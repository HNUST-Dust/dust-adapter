/* SPDX-License-Identifier: Apache-2.0 */

/* IO 后端 facade：初始化、保护、配置、动作派发与跨周期采集。
 * CAN 收发在 can_io.cpp，UART 收发在 uart_io.cpp。 */

#include "io/io_backend.hpp"

#include <algorithm>
#include <cerrno>

#include <zephyr/sys/printk.h>

#include "io/io_internal.hpp"
#include "io/can_profile.hpp"
#if defined(CONFIG_WBR_CONTROL_ONBOARD_IMU_RAW)
#include "drivers/imu_sensor.hpp"
#endif

#if defined(CONFIG_BOARD_OUTPUTS)
extern "C" void imu_heater_observe(float temperature_c);
#endif

namespace dust::ecat {

int AllocateBootIdentity(uint64_t& boot);

bool initialized = false, ready = false, capturing = false;
uint32_t protection_errors = 0;
Configuration configuration;

#if defined(CONFIG_WBR_CONTROL_ONBOARD_IMU_RAW)
modules::ImuSensor onboard_imu;
bool imu_ready = false;
uint32_t imu_sequence = 0;
modules::ImuSensor::Burst latest_imu_burst;
bool latest_imu_valid = false;

/* Sample the local IMU on every poll (independent of the master's PDO
 * configuration) so the heater PID always sees a temperature. The latest
 * burst is published into the process image by CaptureIo. */
void SampleImu() {
    if (!imu_ready) {
        return;
    }
    modules::ImuSensor::Burst burst{};
    if (onboard_imu.TryTakeCompleted(burst)) {
#if defined(CONFIG_BOARD_OUTPUTS)
        imu_heater_observe(burst.temperature_c);
#endif
        latest_imu_burst = burst;
        latest_imu_valid = true;
    }
    (void)onboard_imu.TryStartAsync();
}
#endif

namespace {
void Stop(void*, Error) {
    atomic_clear(&capture_epoch);
    capturing = false;
    UartStop();
    CanStop();
}

void EnterActive(Core& core, uint64_t now) {
    if (capturing) {
        return;
    }
    for (auto& port : ports) {
        k_msgq_purge(&port.receive);
        port.pending.size = 0;
        port.offset = 0;
        port.discontinuity = false;
        atomic_clear(&port.gap);
    }
    if (++epoch == 0 || epoch > INT32_MAX) {
        epoch = 1;
    }
    for (unsigned source = 0; source < 2; ++source) {
        source_deadlines[source] =
            core.active_sources() & (1U << source)
                ? now + dust::can_profile::kCommandTimeoutUs
                : 0;
    }
    capturing = true;
    atomic_set(&capture_epoch, epoch);
}
}  // namespace

int InitializeIo(uint64_t& boot) {
    const bool first = !initialized;
    if (!initialized) {
        for (auto& bus : buses) {
            k_msgq_init(&bus.receive, bus.storage, sizeof(Core::CanEvent), 32);
        }
        for (auto& port : ports) {
            k_msgq_init(&port.receive, port.storage, sizeof(UartBlock), 4);
        }
        const auto fail = [](int error) {
            Stop(nullptr, Error::Transport);
            CanPump(nullptr, Now());
            return error;
        };
        for (const auto& address : dust::can_profile::kTransmitAddresses) {
            bool protected_address = false;
            for (const auto& stop : dust::can_profile::kProtectionFrames) {
                protected_address |= stop.channel == address.channel &&
                                     stop.id == address.id &&
                                     stop.flags == address.flags;
            }
            if (!protected_address || address.flags || address.id > 0x7ff) {
                return fail(-EACCES);
            }
        }
        if (dust::can_profile::kCommandTimeoutUs > kTimeoutUs) {
            return fail(-EINVAL);
        }
        int result = CanInitialize();
        if (result) {
            return fail(result);
        }
        if (dust::can_profile::kProtectOnBoot) {
            Stop(nullptr, Error::State);
            // 先把启动保护交给硬件，再初始化 UART / 持久计数。
            CanPump(nullptr, Now());
        }
        result = UartInitialize();
        if (result) {
            return fail(result);
        }
#if defined(CONFIG_WBR_CONTROL_ONBOARD_IMU_RAW)
        imu_ready = onboard_imu.Init() == 0;
        printk("Onboard IMU: ready=%d\n", imu_ready);
#endif
        initialized = true;
    }
    // 在首次暴露新 boot_id 前清除旧业务，持久化失败不提供可 Arm 的核心。
    if (!first) {
        Stop(nullptr, Error::State);
        CanPump(nullptr, Now());
    }
    if (protection_errors) {
        return -EIO;
    }
    const int result = AllocateBootIdentity(boot);
    ready = !result;
    if (result) {
        Stop(nullptr, Error::Transport);
        CanPump(nullptr, Now());
    }
    return result;
}

Protection IoProtection() {
    return {nullptr, CanValidate, Stop, ready};
}

void ApplyIoLimits(Limits& limits) {
    limits.can_mask = limits.uart_tx_mask = limits.uart_rx_mask = 0;
    CanApplyLimits(limits);
    UartApplyLimits(limits);
#if defined(CONFIG_WBR_CONTROL_ONBOARD_IMU_RAW)
    limits.imu = imu_ready;
#endif
}

bool CanFeedbackCapacityValid(const Configuration& value) {
    return CanCapacityValid(value);
}

void ConfigureIo(const Configuration& value) {
    configuration = value;
    CanConfigure(value);
}

void DispatchIo(Core& core, const Actions& actions, uint64_t now) {
    if (actions.status_sequence) {
        const unsigned target = actions.status_target & 3,
                       page = actions.status_target >> 2;
        const unsigned channel = actions.status_channel;
        std::array<uint32_t, 3> values{};
        int result = 0;
        if (target == 0 && !channel) {
            if (!page) {
                values = {uint32_t(core.state()),
                          dust::can_profile::kProtectionSignature,
                          dust::can_profile::kCommandTimeoutUs};
            } else if (page == 1) {
                values = {protection_errors, uint32_t(ready),
                          uint32_t(initialized)};
            } else {
                result = -ENOTSUP;
            }
        } else if (target == 1 && channel < 4) {
            result = CanStatus(channel, page, values);
        } else if (target == 2 && channel >= 1 && channel <= 6) {
            result = UartStatus(channel - 1, page, values);
        } else {
            result = -EINVAL;
        }
        core.CompleteStatus(actions.status_sequence, result, values);
    }

    if (core.state() != BusinessState::Active) {
        return;
    }
    if (CanAnyFault() || UartAnyFault()) {
        core.Protect(Error::Transport);
        return;
    }
    if (capturing) {
        for (auto deadline : source_deadlines) {
            if (deadline && now >= deadline) {
                core.Protect(Error::Lease);
                return;
            }
        }
    }
    EnterActive(core, now);
    if (!CanDispatch(core, actions, now)) {
        return;
    }
    (void)UartDispatch(core, actions, now);
}

void ServiceIo(Core& core, uint64_t now) {
#if defined(CONFIG_WBR_CONTROL_ONBOARD_IMU_RAW)
    SampleImu();
#endif

    if (core.state() == BusinessState::Active) {
        EnterActive(core, now);
        for (auto deadline : source_deadlines) {
            if (deadline && now >= deadline) {
                core.Protect(Error::Lease);
                return;
            }
        }
    }
    if (core.state() == BusinessState::Ready ||
        core.state() == BusinessState::Active) {
        if (CanAnyFault() || UartAnyFault()) {
            core.Protect(Error::Transport);
            return;
        }
    }
    // 保护状态也需要把不可覆盖的本地保护帧交给控制器。
    CanPump(&core, now);
    if (core.state() != BusinessState::Active) {
        return;
    }
    UartService(core, now);
}

bool CanReceivePending() {
    return true;  // Baseline and single-thread mode must still service queues.
}

bool ServiceCanReceive() {
    return CanDrainReceive();
}

void CaptureCanFeedback(Core::Feedback& feedback) {
    CanCaptureFeedback(feedback);
}

void CaptureIo(Core& core, Core::Feedback& feedback, uint64_t now) {
    (void)ServiceCanReceive();
    CaptureCanFeedback(feedback);
    UartCapture(core, feedback, now);
#if defined(CONFIG_WBR_CONTROL_ONBOARD_IMU_RAW)
    /* SampleImu() already feeds the heater; publish the newest burst here. */
    if (latest_imu_valid && configuration.imu) {
        feedback.imu_valid = true;
        feedback.imu_sequence = ++imu_sequence;
        feedback.imu_capture_us =
            k_cyc_to_us_floor32(latest_imu_burst.data_ready_cycle);
        feedback.temperature = latest_imu_burst.temperature_c;
        std::copy(latest_imu_burst.rx,
                  latest_imu_burst.rx + sizeof(latest_imu_burst.rx),
                  feedback.imu_raw.begin());
        latest_imu_valid = false;
    }
#endif
}

}  // namespace dust::ecat

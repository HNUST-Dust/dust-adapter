/* SPDX-License-Identifier: Apache-2.0 */

/**
 * @file can_port.cpp
 * @brief 配置驱动的 CAN 过滤和每总线 FIFO 发送线程。
 */
#include "drivers/can_port.hpp"

#include <drivers/can/hpmicro_can_batch.h>
#include <zephyr/drivers/can.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

#include <cstring>

#include "drivers/firmware_can_profile.hpp"

namespace {
constexpr int kCanTxPriority = 6;
constexpr unsigned kBusCount = 4;
constexpr unsigned kDepth = 32;
constexpr unsigned kBatchSize = 3;

/**
 * @brief 每总线 FIFO、硬件批次及工作线程同步状态。
 */
struct Bus {
    const device* dev = nullptr;
    uint8_t index = 0;
    k_mutex mutex;
    k_sem wake;
    k_msgq queue;
    alignas(4) char storage[kDepth * sizeof(dust::wire::CanFrame)];
    can_frame pending[kBatchSize]{};
    uint8_t pending_count = 0;
    atomic_t received{};
    atomic_t submitted{};
    atomic_t completed{};
    atomic_t errors{};
    atomic_t dropped{};
    uint32_t next_receive_sequence = 0;
};
Bus buses[kBusCount];
k_thread threads[kBusCount];
K_THREAD_STACK_ARRAY_DEFINE(stacks, kBusCount, 1536);
platform::CanReceiveCallback receive_callback;
atomic_t fault;
K_MUTEX_DEFINE(admission_mutex);
const device* FindDevice(unsigned bus) {
    switch (bus) {
#if DT_NODE_HAS_STATUS(DT_ALIAS(dust_can_0), okay)
        case 0:
            return DEVICE_DT_GET(DT_ALIAS(dust_can_0));
#endif
#if DT_NODE_HAS_STATUS(DT_ALIAS(dust_can_1), okay)
        case 1:
            return DEVICE_DT_GET(DT_ALIAS(dust_can_1));
#endif
#if DT_NODE_HAS_STATUS(DT_ALIAS(dust_can_2), okay)
        case 2:
            return DEVICE_DT_GET(DT_ALIAS(dust_can_2));
#endif
#if DT_NODE_HAS_STATUS(DT_ALIAS(dust_can_3), okay)
        case 3:
            return DEVICE_DT_GET(DT_ALIAS(dust_can_3));
#endif
        default:
            return nullptr;
    }
}

/**
 * @brief 将 CAN 驱动回调转换为带时间戳的记录，并调用接收消费者。
 */
void CanRxCallback(const device*, can_frame* frame, void* user) {
    auto& bus = *static_cast<Bus*>(user);
    platform::CanReceiveFrame received{};
    received.timestamp_us = k_cyc_to_us_floor64(k_cycle_get_64());
    received.sequence = bus.next_receive_sequence++;
    received.record.channel = bus.index;
    received.record.id = frame->id;
    received.record.length = frame->dlc;
    received.record.flags =
        ((frame->flags & CAN_FRAME_IDE) ? dust::wire::kExtended : 0) |
        ((frame->flags & CAN_FRAME_RTR) ? dust::wire::kRemote : 0);
    if (frame->dlc > 8 || (frame->flags & CAN_FRAME_FDF)) {
        return;
    }
    if (!(frame->flags & CAN_FRAME_RTR)) {
        std::memcpy(received.record.data, frame->data, frame->dlc);
    }
    atomic_inc(&bus.received);
    if (receive_callback) {
        receive_callback(received);
    }
}

/**
 * @brief 记录 CAN 完成状态、锁存错误并唤醒总线工作线程。
 */
void CanTxCallback(const device*, int error, void* user) {
    auto& bus = *static_cast<Bus*>(user);
    if (error) {
        atomic_inc(&bus.errors);
        atomic_set(&fault, 1);
    } else {
        // 统计已完成的硬件批次，不是设备确认次数。
        atomic_inc(&bus.completed);
    }
    k_sem_give(&bus.wake);
}

/**
 * @brief 在专用线程中读取一条 CAN 总线的 FIFO，提交有界硬件批次。
 */
void CanTxLoop(void* user, void*, void*) {
    auto& bus = *static_cast<Bus*>(user);
    for (;;) {
        // 即使硬件完成中断丢失，
        // 重试等待仍有时间上限。
        k_sem_take(&bus.wake, K_MSEC(1));
        k_mutex_lock(&bus.mutex, K_FOREVER);
        if (!bus.pending_count) {
            dust::wire::CanFrame record{};
            while (bus.pending_count < kBatchSize &&
                   k_msgq_get(&bus.queue, &record, K_NO_WAIT) == 0) {
                auto& frame = bus.pending[bus.pending_count++];
                frame = {};
                frame.id = record.id;
                frame.dlc = record.length;
                frame.flags =
                    ((record.flags & dust::wire::kExtended) ? CAN_FRAME_IDE
                                                            : 0) |
                    ((record.flags & dust::wire::kRemote) ? CAN_FRAME_RTR : 0);
                std::memcpy(frame.data, record.data, 8);
            }
        }
        if (bus.pending_count) {
            const int rc =
                hpm_can_send_batch(bus.dev, bus.pending, bus.pending_count,
                                   K_NO_WAIT, CanTxCallback, &bus);
            if (!rc) {
                atomic_add(&bus.submitted, bus.pending_count);
                bus.pending_count = 0;
                if (k_msgq_num_used_get(&bus.queue)) {
                    k_sem_give(&bus.wake);
                }
            } else if (rc != -EAGAIN && rc != -EBUSY) {
                atomic_inc(&bus.errors);
                atomic_set(&fault, 1);
                bus.pending_count = 0;
            }
        }
        k_mutex_unlock(&bus.mutex);
    }
}

/**
 * @brief 校验全部记录，锁定接纳流程及总线队列，再原子追加或替换。
 * @return 接纳成功返回零，否则返回负 errno；硬件完成仍为异步过程。
 */
int Publish(const dust::wire::CanFrame* records, size_t count, bool replace) {
    if (count > (replace ? 32U : dust::wire::kMaxRecords) ||
        (count && !records)) {
        return -EINVAL;
    }
    unsigned required[kBusCount]{};
    for (size_t i = 0; i < count; ++i) {
        if (!platform::CanAvailable(records[i].channel)) {
            return -ENODEV;
        }
        if (!dust::can_profile::AllowsTransmit(records[i])) {
            return -EACCES;
        }
        ++required[records[i].channel];
    }
    k_mutex_lock(&admission_mutex, K_FOREVER);
    for (auto& bus : buses) {
        if (bus.dev) {
            k_mutex_lock(&bus.mutex, K_FOREVER);
        }
    }
    int result = 0;
    if (!replace) {
        for (auto& bus : buses) {
            if (bus.dev &&
                required[bus.index] > k_msgq_num_free_get(&bus.queue)) {
                atomic_inc(&bus.dropped);
                result = -ENOSPC;
            }
        }
    }
    if (!result) {
        for (auto& bus : buses) {
            if (bus.dev && replace) {
                atomic_add(&bus.dropped,
                           k_msgq_num_used_get(&bus.queue) + bus.pending_count);
                k_msgq_purge(&bus.queue);
                bus.pending_count = 0;
            }
        }
        for (size_t i = 0; i < count; ++i) {
            k_msgq_put(&buses[records[i].channel].queue, records + i,
                       K_NO_WAIT);
        }
    }
    for (auto& bus : buses) {
        if (bus.dev) {
            k_mutex_unlock(&bus.mutex);
            k_sem_give(&bus.wake);
        }
    }
    k_mutex_unlock(&admission_mutex);
    return result;
}
}  // namespace

namespace platform {

/**
 * @brief 初始化已配置总线、精确 RX 过滤器和 TX 工作线程；返回零或负 errno。
 */
int InitializeCanPorts(CanReceiveCallback callback) {
    receive_callback = callback;
    for (const auto& configured : dust::can_profile::kBuses) {
        const unsigned i = configured.channel;
        if (i >= kBusCount) {
            return -EINVAL;
        }
        auto& bus = buses[i];
        bus.index = i;
        bus.dev = FindDevice(i);
        if (!bus.dev || !device_is_ready(bus.dev)) {
            bus.dev = nullptr;
            return -ENODEV;
        }
        k_mutex_init(&bus.mutex);
        k_sem_init(&bus.wake, 0, 1);
        k_msgq_init(&bus.queue, bus.storage, sizeof(dust::wire::CanFrame),
                    kDepth);
        int rc = can_set_mode(bus.dev, CAN_MODE_NORMAL);
        if (rc) {
            return rc;
        }
        // 这是板级能力，不是设备或电机属性。
        rc = can_set_bitrate(bus.dev, configured.bitrate);
        if (rc) {
            return rc;
        }
        rc = can_start(bus.dev);
        if (rc && rc != -EALREADY) {
            return rc;
        }
        for (const auto& address : dust::can_profile::kReceiveFilters) {
            if (address.channel != i) {
                continue;
            }
            const can_filter filter{
                .id = address.id,
                .mask = (address.flags & dust::wire::kExtended) ? 0x1fffffffU
                                                                : 0x7ffU,
                .flags = uint8_t(
                    ((address.flags & dust::wire::kExtended) ? CAN_FILTER_IDE
                                                             : 0) |
                    ((address.flags & dust::wire::kRemote) ? CAN_FRAME_RTR
                                                           : 0))};
            rc = can_add_rx_filter(bus.dev, CanRxCallback, &bus, &filter);
            if (rc < 0) {
                return rc;
            }
        }
        k_thread_create(&threads[i], stacks[i],
                        K_THREAD_STACK_SIZEOF(stacks[i]), CanTxLoop, &bus,
                        nullptr, nullptr, kCanTxPriority, 0, K_NO_WAIT);
    }
    return CanChannelMask() ? 0 : -ENODEV;
}

/**
 * @brief 返回总线编号是否在范围内且已初始化。
 */
bool CanAvailable(uint8_t bus) {
    return bus < kBusCount && buses[bus].dev;
}

/**
 * @brief 为每个已初始化的逻辑 CAN 通道返回一个可用性位。
 */
uint32_t CanChannelMask() {
    uint32_t mask = 0;
    for (unsigned i = 0; i < kBusCount; ++i) {
        if (CanAvailable(i)) {
            mask |= 1U << i;
        }
    }
    return mask;
}

/**
 * @brief 在 FIFO 接纳前校验整个批次；返回零或负 errno。
 */
int SubmitCanRecords(const dust::wire::CanFrame* records, size_t count) {
    return Publish(records, count, false);
}

/**
 * @brief 丢弃排队的业务流量并接纳保护记录。
 * @note 已提交到硬件的帧无法撤回。
 */
int ReplaceCanTraffic(const dust::wire::CanFrame* records, size_t count) {
    return Publish(records, count, true);
}

/**
 * @brief 以原子操作读取并清除锁存的 CAN 输出故障。
 */
bool TakeCanFault() {
    return atomic_cas(&fault, 1, 0);
}

/**
 * @brief 返回总线传输和控制器计数器；无效总线返回全零计数。
 */
CanStats ReadCanStats(uint8_t bus) {
    CanStats stats{};
    if (!CanAvailable(bus)) {
        return stats;
    }
    auto& b = buses[bus];
    stats.received = atomic_get(&b.received);
    stats.submitted = atomic_get(&b.submitted);
    stats.completed = atomic_get(&b.completed);
    stats.errors = atomic_get(&b.errors);
    stats.dropped = atomic_get(&b.dropped);
    stats.queue_depth = k_msgq_num_used_get(&b.queue);
    can_state state;
    can_bus_err_cnt errors{};
    if (!can_get_state(b.dev, &state, &errors)) {
        stats.state = uint32_t(state);
        stats.tx_errors = errors.tx_err_cnt;
        stats.rx_errors = errors.rx_err_cnt;
    }
    return stats;
}
}  // namespace platform

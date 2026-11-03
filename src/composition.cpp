/* SPDX-License-Identifier: Apache-2.0 */

#include "composition.hpp"

#include <array>
#include <cstddef>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include <can_profile_generated.hpp>
#include <gateway_config.h>

#include "adapters/esc/esc_hw.h"
#include "common/types.hpp"
#include "domain/can/can_topology.hpp"
#include "identity.hpp"
#if defined(CONFIG_BUZZER)
#include "drivers/pwm/buzzer.h"
#endif

namespace app {
namespace {

struct BoardData {
    std::array<ecat::CanBusConfig, 4> can_buses{};
    size_t can_bus_count = 0;

    std::array<ecat::UartPortConfig, 6> uart_ports{};

    std::array<ecat::CanFilterConfig, 64> filters{};
    size_t filter_count = 0;

    std::array<ecat::CanTxAddress, 64> tx_addresses{};
    size_t tx_count = 0;

    std::array<ecat::CanFrame, 32> protection_frames{};
    size_t protection_count = 0;

    ecat::CanTopology can_topology{};
    ecat::HeaterConfig heater{};
    ecat::SscBoardConfig ssc{};
};

const device* CanDevice(uint8_t channel) {
    switch (channel) {
        case 0:
            return DEVICE_DT_GET(DT_ALIAS(dust_can_0));
        case 1:
            return DEVICE_DT_GET(DT_ALIAS(dust_can_1));
        case 2:
            return DEVICE_DT_GET(DT_ALIAS(dust_can_2));
        default:
            return DEVICE_DT_GET(DT_ALIAS(dust_can_3));
    }
}

const device* UartDevice(uint8_t index) {
    switch (index) {
        case 0:
            return DEVICE_DT_GET(DT_ALIAS(dust_uart_1));
        case 1:
            return DEVICE_DT_GET(DT_ALIAS(dust_uart_2));
        case 2:
            return DEVICE_DT_GET(DT_ALIAS(dust_uart_3));
        case 3:
            return DEVICE_DT_GET(DT_ALIAS(dust_uart_4));
        case 4:
            return DEVICE_DT_GET(DT_ALIAS(dust_uart_5));
        default:
            return DEVICE_DT_GET(DT_ALIAS(dust_uart_6));
    }
}

BoardData BuildBoard() {
    BoardData data;

    for (const auto& bus : can_profile::kBuses) {
        if (data.can_bus_count == data.can_buses.size()) {
            break;
        }
        data.can_buses[data.can_bus_count++] = {CanDevice(bus.channel),
                                               bus.channel, bus.bitrate};
    }

    for (size_t index = 0; index < data.uart_ports.size(); ++index) {
        data.uart_ports[index] = {UartDevice(uint8_t(index)), uint8_t(index),
                                  index == 4, index == 4};
    }

    for (const auto& filter : can_profile::kReceiveFilters) {
        if (data.filter_count == data.filters.size()) {
            break;
        }
        data.filters[data.filter_count++] = {filter.channel, filter.id, 0x7ff};
    }

    for (const auto& address : can_profile::kTransmitAddresses) {
        if (data.tx_count == data.tx_addresses.size()) {
            break;
        }
        data.tx_addresses[data.tx_count++] = {address.channel, address.id,
                                             address.flags};
    }

    for (const auto& frame : can_profile::kProtectionFrames) {
        if (data.protection_count == data.protection_frames.size()) {
            break;
        }
        ecat::CanFrame value;
        value.bus = frame.channel;
        value.length = frame.length;
        value.id = frame.id;
        for (size_t i = 0; i < value.data.size(); ++i) {
            value.data[i] = frame.data[i];
        }
        data.protection_frames[data.protection_count++] = value;
    }

    data.can_topology.command_timeout_us = can_profile::kCommandTimeoutUs;
    data.can_topology.protection_signature =
        can_profile::kProtectionSignature;
    data.can_topology.protect_on_boot = can_profile::kProtectOnBoot;

#if defined(CONFIG_BOARD_OUTPUTS)
    data.heater.enabled = IS_ENABLED(CONFIG_IMU_HEATER_ENABLE);
    data.heater.target_c = float(CONFIG_IMU_TARGET_C);
    data.heater.kp = float(CONFIG_IMU_HEATER_KP_MILLI) / 1000.0f;
    data.heater.ki = float(CONFIG_IMU_HEATER_KI_MILLI) / 1000.0f;
    data.heater.kd = float(CONFIG_IMU_HEATER_KD_MILLI) / 1000.0f;
    data.heater.max_duty =
        float(CONFIG_IMU_HEATER_MAX_PERMILLE) / 1000.0f;
    data.heater.period_ms = CONFIG_IMU_HEATER_PERIOD_MS;
#endif

    data.ssc.rx_max = MAX_PD_OUTPUT_SIZE;
    data.ssc.tx_max = MAX_PD_INPUT_SIZE;
    data.ssc.ram_end = ECAT_RAM_END;
    data.ssc.rx_address = ECAT_RX_ADDRESS;
    data.ssc.tx_address = ECAT_TX_ADDRESS;
    return data;
}

const BoardData& Board() {
    static BoardData data = BuildBoard();
    /* 拓扑中的指针指向本静态实例的数组，初始化后再接线。 */
    data.can_topology.tx_addresses = data.tx_addresses.data();
    data.can_topology.tx_count = data.tx_count;
    data.can_topology.filters = data.filters.data();
    data.can_topology.filter_count = data.filter_count;
    data.can_topology.protection_frames = data.protection_frames.data();
    data.can_topology.protection_count = data.protection_count;
    return data;
}

}  // namespace

Composition::Composition()
    : can_bus_(Board().can_buses.data(), Board().can_bus_count),
      uart_port_(Board().uart_ports.data(), Board().uart_ports.size()),
      outputs_(Board().heater),
      session_(0),
      can_(can_bus_, Board().can_topology),
      uart_(uart_port_),
      feedback_(can_, uart_, imu_),
      io_(can_, uart_, feedback_),
      protection_(session_, can_, uart_),
      stack_(session_, layout_, io_, protection_, Board().ssc),
      lifecycle_(stack_, &ecat::AllocateBootIdentity, session_, protection_,
                 io_),
      scheduler_(lifecycle_, stack_, session_, io_, feedback_, outputs_) {}

int Composition::Init() {
    if (esc_init() != 0) {
        printk("EtherCAT ESC init failed\n");
        return -1;
    }
#if defined(CONFIG_BUZZER)
    const int buzzer_rc = buzzer_init();
    if (buzzer_rc) {
        printk("Buzzer init failed: %d\n", buzzer_rc);
    }
#endif

    can_.AttachFaultSink(protection_.Sink());
    uart_.AttachFaultSink(protection_.Sink());
    io_.AttachFaultSink(protection_.Sink());

    if (lifecycle_.Boot() != 0) {
        printk("Gateway boot failed\n");
        return -1;
    }
    if (outputs_.Init() != 0) {
        printk("Board outputs init failed\n");
    }
    lifecycle_.Enable();
    printk("DUST HPM5E31 DTC1 LAB ready: VID=0 PID=0x44544331; CAN/UART IO "
           "installed\n");
    return 0;
}

void Composition::Run() {
    for (;;) {
        const ecat::Scheduler::Step step = scheduler_.RunOnce();
        if (step == ecat::Scheduler::Step::Fatal) {
            printk("Gateway fatal; IRQs remain disabled\n");
            return;
        }
        if (step == ecat::Scheduler::Step::Active) {
            k_busy_wait(10);
        } else {
            k_usleep(100);
        }
    }
}

}  // namespace app

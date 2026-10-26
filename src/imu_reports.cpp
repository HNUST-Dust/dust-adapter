/* SPDX-License-Identifier: Apache-2.0 */
#include "imu_reports.hpp"

#include <zephyr/kernel.h>

#include <cstring>

#include "drivers/imu_heater.hpp"
#include "drivers/imu_sensor.hpp"
#include "usb_reports.hpp"
namespace {
namespace wire = dust::wire;
constexpr int kImuPriority = 4;
K_THREAD_STACK_DEFINE(imu_stack, 2048);
k_thread imu_thread;
modules::ImuSensor onboard;

/**
 * @brief 在专用线程中读取原始 IMU 突发数据，发布携带会话标识的数据包。
 */
void ImuLoop(void*, void*, void*) {
    uint32_t sequence = 0;
    for (;;) {
        modules::ImuSensor::Burst burst{};
        if (onboard.TryTakeCompleted(burst)) {
            platform::ObserveImuTemperature(burst.temperature_c);
            wire::UsbInPacket packet{};
            const uint32_t now = k_cyc_to_us_floor64(k_cycle_get_64());
            wire::ImuSampleReport report{};
            report.header.session_id = gateway::CurrentUsbReportSession();
            const uint32_t age =
                k_cyc_to_us_floor32(k_cycle_get_32() - burst.data_ready_cycle);
            report.sample_timestamp_us = now - age;
            report.sample_sequence = sequence++;
            std::memcpy(report.spi_bytes.data(), burst.rx, sizeof(burst.rx));
            report.temperature_c = burst.temperature_c;
            wire::Encode(report, packet);
            gateway::QueueSensorReportForUsb(packet);
        }
        (void)onboard.TryStartAsync();
        k_usleep(100);
    }
}

}  // namespace
namespace gateway {
uint32_t StartImuReports() {
    uint32_t capabilities = 0;
    if (onboard.Init() == 0) {
        capabilities |= 8;
        k_thread_create(&imu_thread, imu_stack,
                        K_THREAD_STACK_SIZEOF(imu_stack), ImuLoop, nullptr,
                        nullptr, nullptr, kImuPriority, 0, K_NO_WAIT);
    }
    return capabilities;
}
}  // namespace gateway

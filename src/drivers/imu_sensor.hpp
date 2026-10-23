/* SPDX-License-Identifier: Apache-2.0 */

/**
 * @file imu_sensor.hpp
 * @brief HXY 板载 IMU 传输与非阻塞采样；原始突发格式固定。
 */

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

namespace modules {

/**
 * HXY 板载 IMU 传输与非阻塞采样；原始突发格式固定。
 */
class ImuSensor final {
   public:
    /* 0x0C..0x23（含端点）：加速度、角速度及温度寄存器。 */
    static constexpr size_t kBurstTransferSize = 25U;

    /**
     * @brief 原始 SPI 突发数据、DRDY 时间戳及解码后的传感器温度。
     */
    struct Burst {
        uint8_t rx[kBurstTransferSize];
        uint32_t data_ready_cycle;
        float temperature_c;
    };

    /**
     * 初始化传感器、SPI 传输和数据就绪中断。
     */
    int Init();

    /**
     * 无等待复制已完成的 DMA 传输。
     */
    bool TryTakeCompleted(Burst& burst);

    /**
     * 无等待提交最新待处理的 DRDY 样本。
     */
    int TryStartAsync();

   private:
    /**
     * @brief 在 GPIO 中断上下文中记录 DRDY 周期时间戳。
     */
    static void DataReadyCallback(const struct device* port,
                                  struct gpio_callback* callback,
                                  gpio_port_pins_t pins);

    /**
     * @brief 锁存 SPI 完成状态供后续读取，不发布样本。
     */
    static void TransferCallback(const struct device* dev, int status,
                                 void* userdata);

    /**
     * @brief 配置 SPI、重置传感器寄存器并启用数据就绪 GPIO；返回零或负 errno。
     */
    int InitializeHardware();

    /**
     * 无等待替换队列中过时的 DRDY 时间戳。
     */
    void QueueLatestDataReady(uint32_t data_ready_cycle);

    /**
     * @brief 将一个传感器寄存器读入 value；返回零或负 SPI errno。
     */
    int ReadRegister(uint8_t reg, uint8_t* value);

    /**
     * @brief 写入一个传感器寄存器；返回零或负 SPI errno。
     */
    int WriteRegister(uint8_t reg, uint8_t value);

    /**
     * @brief 为选定样本启动异步 DMA 突发传输；返回零或负 errno。
     */
    int StartBurstRead();

    const struct device* spi_dev_ = nullptr;
    struct gpio_dt_spec cs_ = {};
    struct gpio_dt_spec data_ready_ = {};
    struct gpio_callback data_ready_callback_ = {};
    struct k_msgq data_ready_queue_ = {};
    char data_ready_queue_buffer_[sizeof(uint32_t)] = {};
    atomic_t transfer_in_flight_ = ATOMIC_INIT(0);
    atomic_t transfer_status_ = ATOMIC_INIT(0);
    atomic_t transfer_complete_ = ATOMIC_INIT(0);
    atomic_t timeout_recovery_started_ = ATOMIC_INIT(0);
    uint32_t active_data_ready_cycle_ = 0U;
    uint32_t transfer_start_cycle_ = 0U;
};

}  // namespace modules

/* SPDX-License-Identifier: Apache-2.0 */
/** @file onboard_imu.hpp
 * @brief Board IMU transport and nonblocking sample acquisition.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

namespace modules {

/** Board-mounted IMU transport and non-blocking sample acquisition. */
class OnboardImu final {
   public:
    /* 0x0C..0x23 inclusive: accel/gyro plus the temperature registers. */
    static constexpr size_t kBurstTransferSize = 25U;

    /** @brief Raw SPI burst, DRDY timestamp and decoded sensor temperature. */
    struct Burst {
        uint8_t rx[kBurstTransferSize];
        uint32_t data_ready_cycle;
        float temperature_c;
    };

    /** Initialize the sensor, SPI transport, and data-ready interrupt. */
    int Init();
    /** Copy a completed DMA transfer without waiting. */
    bool TryTakeCompleted(Burst& burst);
    /** Submit the latest pending DRDY sample without waiting. */
    int TryStartAsync();

   private:
    /**
     * @brief Capture the DRDY cycle timestamp in GPIO interrupt context.
     */
    static void DataReadyCallback(const struct device* port,
                                  struct gpio_callback* callback,
                                  gpio_port_pins_t pins);
    /**
     * @brief Latch SPI completion status for later consumption; does not
     * publish a sample.
     */
    static void TransferCallback(const struct device* dev, int status,
                                 void* userdata);

    /**
     * @brief Configure SPI, reset sensor registers and enable data-ready GPIO;
     * return zero or negative errno.
     */
    int InitializeHardware();
    /** Replace any stale queued DRDY timestamp without waiting. */
    void QueueLatestDataReady(uint32_t data_ready_cycle);
    /**
     * @brief Read one sensor register into value; return zero or negative SPI
     * errno.
     */
    int ReadRegister(uint8_t reg, uint8_t* value);
    /**
     * @brief Write one sensor register; return zero or negative SPI errno.
     */
    int WriteRegister(uint8_t reg, uint8_t value);
    /**
     * @brief Start an asynchronous DMA burst for the selected sample; return
     * zero or negative errno.
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

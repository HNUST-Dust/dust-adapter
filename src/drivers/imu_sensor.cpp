/* SPDX-License-Identifier: Apache-2.0 */

/**
 * @file imu_sensor.cpp
 * @brief 实现板载 IMU 初始化、INT1 处理和异步 SPI 采集。
 */

#include "drivers/imu_sensor.hpp"

#include <errno.h>
#include <string.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/linker/section_tags.h>
#include <zephyr/sys/util.h>
#include <zephyr/timing/timing.h>

#if defined(CONFIG_WBR_CONTROL_ONBOARD_IMU_RAW)

#define ONBOARD_IMU_SPI_NODE DT_CHOSEN(wbr_control_onboard_imu_spi)
#define ONBOARD_IMU_CS_NODE DT_ALIAS(onboard_imu_cs)
#define ONBOARD_IMU_DRDY_NODE DT_ALIAS(onboard_imu_drdy)

#if !DT_HAS_CHOSEN(wbr_control_onboard_imu_spi)
#error "wbr-control,onboard-imu-spi must select the onboard IMU SPI controller"
#endif
#if !DT_NODE_HAS_STATUS(ONBOARD_IMU_SPI_NODE, okay)
#error "The selected onboard IMU SPI controller must be enabled"
#endif
#if !DT_NODE_EXISTS(ONBOARD_IMU_CS_NODE)
#error "Define devicetree alias onboard-imu-cs"
#endif
#if !DT_NODE_EXISTS(ONBOARD_IMU_DRDY_NODE)
#error "Define devicetree alias onboard-imu-drdy with the PCB INT1 GPIO"
#endif

namespace {
constexpr uint8_t kReadMask = 0x80U;
constexpr uint8_t kWhoAmIRegister = 0x01U;
constexpr uint8_t kWhoAmIExpected = 0x6AU;
constexpr uint8_t kComConfigRegister = 0x05U;
constexpr uint8_t kIntConfig1Register = 0x06U;
constexpr uint8_t kAccelDataStartRegister = 0x0CU;
constexpr uint8_t kAccelConfigRegister = 0x40U;
constexpr uint8_t kAccelRangeRegister = 0x41U;
constexpr uint8_t kGyroConfigRegister = 0x42U;
constexpr uint8_t kGyroRangeRegister = 0x43U;
constexpr uint8_t kPowerControlRegister = 0x7DU;
constexpr uint8_t kPowerAllSensors = 0x0EU;
constexpr uint8_t kBlockUpdateAndAutoIncrement = 0x50U;
constexpr uint8_t kDataReadyGyroOnInt1 = 0x03U;
constexpr uint8_t kAccelHighPerformance1600Hz = 0xACU;
constexpr uint8_t kAccelRange8G = 0x02U;
constexpr uint8_t kGyroHighPerformance1600Hz = 0xACU;
constexpr uint8_t kGyroRange2000Dps = 0x00U;
constexpr size_t kBurstPayloadSize = 24U;
constexpr size_t kBurstTransferSize = kBurstPayloadSize + 1U;
constexpr uint32_t kTransferTimeoutUs = 1500U;

uint8_t __nocache __aligned(CONFIG_DUST_DMA_BUFFER_ALIGNMENT)
    g_onboard_imu_tx[kBurstTransferSize];
uint8_t __nocache __aligned(CONFIG_DUST_DMA_BUFFER_ALIGNMENT)
    g_onboard_imu_rx[kBurstTransferSize];
uint8_t __nocache __aligned(CONFIG_DUST_DMA_BUFFER_ALIGNMENT)
    g_onboard_imu_control_tx[2];
uint8_t __nocache __aligned(CONFIG_DUST_DMA_BUFFER_ALIGNMENT)
    g_onboard_imu_control_rx[2];

/**
 * @brief 将两个可读传感器字节解码为大端有符号值。
 */
int16_t DecodeBigEndian(const uint8_t* bytes) {
    return static_cast<int16_t>((static_cast<uint16_t>(bytes[0]) << 8U) |
                                static_cast<uint16_t>(bytes[1]));
}

struct spi_buf g_onboard_imu_tx_buf = {
    .buf = g_onboard_imu_tx,
    .len = sizeof(g_onboard_imu_tx),
};
struct spi_buf g_onboard_imu_rx_buf = {
    .buf = g_onboard_imu_rx,
    .len = sizeof(g_onboard_imu_rx),
};
const struct spi_buf_set g_onboard_imu_tx_set = {
    .buffers = &g_onboard_imu_tx_buf,
    .count = 1U,
};
const struct spi_buf_set g_onboard_imu_rx_set = {
    .buffers = &g_onboard_imu_rx_buf,
    .count = 1U,
};
const struct spi_config g_onboard_imu_spi_config = {
    .frequency = CONFIG_WBR_CONTROL_ONBOARD_IMU_SPI_FREQUENCY_HZ,
    .operation = SPI_OP_MODE_MASTER | SPI_WORD_SET(8) | SPI_TRANSFER_MSB,
    .slave = 0U,
    .cs =
        {
            .gpio = GPIO_DT_SPEC_GET(ONBOARD_IMU_CS_NODE, gpios),
            .delay = 0U,
        },
};
}  // namespace

namespace modules {

/**
 * @brief 初始化传感器寄存器、SPI 和 DRDY 状态；返回零或负 errno。
 */
int ImuSensor::Init() {
    spi_dev_ = DEVICE_DT_GET(ONBOARD_IMU_SPI_NODE);
    cs_ = GPIO_DT_SPEC_GET(ONBOARD_IMU_CS_NODE, gpios);
    data_ready_ = GPIO_DT_SPEC_GET(ONBOARD_IMU_DRDY_NODE, gpios);
    if (!device_is_ready(spi_dev_) || !gpio_is_ready_dt(&cs_) ||
        !gpio_is_ready_dt(&data_ready_)) {
        return -ENODEV;
    }
    k_msgq_init(&data_ready_queue_, data_ready_queue_buffer_, sizeof(uint32_t),
                1U);
    return InitializeHardware();
}

/**
 * @brief 配置 SPI、重置传感器寄存器并启用数据就绪 GPIO；返回零或负 errno。
 */
int ImuSensor::InitializeHardware() {
    int rc = gpio_pin_configure_dt(&cs_, GPIO_OUTPUT_INACTIVE);
    if (rc != 0) {
        return rc;
    }
    rc = gpio_pin_configure_dt(&data_ready_, GPIO_INPUT);
    if (rc != 0) {
        return rc;
    }

    uint8_t who_am_i = 0U;
    rc = ReadRegister(kWhoAmIRegister, &who_am_i);
    if ((rc != 0) || (who_am_i != kWhoAmIExpected)) {
        return (rc != 0) ? rc : -ENODEV;
    }

    rc = WriteRegister(kPowerControlRegister, kPowerAllSensors);
    if (rc != 0) {
        return rc;
    }
    k_sleep(K_MSEC(10));

    const struct {
        uint8_t reg;
        uint8_t value;
    } configuration[] = {
        {kComConfigRegister, kBlockUpdateAndAutoIncrement},
        {kAccelRangeRegister, kAccelRange8G},
        {kGyroRangeRegister, kGyroRange2000Dps},
        {kAccelConfigRegister, kAccelHighPerformance1600Hz},
        {kGyroConfigRegister, kGyroHighPerformance1600Hz},
    };
    for (const auto& item : configuration) {
        rc = WriteRegister(item.reg, item.value);
        if (rc != 0) {
            return rc;
        }
        k_sleep(K_MSEC(1));
        uint8_t readback = 0U;
        rc = ReadRegister(item.reg, &readback);
        if ((rc != 0) || (readback != item.value)) {
            return (rc != 0) ? rc : -EIO;
        }
    }

    gpio_init_callback(&data_ready_callback_, DataReadyCallback,
                       BIT(data_ready_.pin));
    rc = gpio_add_callback(data_ready_.port, &data_ready_callback_);
    if (rc != 0) {
        return rc;
    }
    rc = gpio_pin_interrupt_configure_dt(&data_ready_, GPIO_INT_EDGE_TO_ACTIVE);
    if (rc != 0) {
        return rc;
    }

    /* MCU 回调就绪后才开启 DRDY 路由，避免首个边沿在传感器配置与 GPIO
     * 中断设置之间丢失。 */
    rc = WriteRegister(kIntConfig1Register, kDataReadyGyroOnInt1);
    if (rc != 0) {
        return rc;
    }
    k_sleep(K_MSEC(1));
    uint8_t int_config_readback = 0U;
    rc = ReadRegister(kIntConfig1Register, &int_config_readback);
    if ((rc != 0) || (int_config_readback != kDataReadyGyroOnInt1)) {
        return (rc != 0) ? rc : -EIO;
    }

    /* 边沿检测生效前 DRDY
     * 可能已经置位。此时先启动一次非阻塞采集，读取输出寄存器以释放
     * INT1，让后续上升沿正常到达。 */
    const int data_ready_level = gpio_pin_get_dt(&data_ready_);
    if (data_ready_level < 0) {
        return data_ready_level;
    }
    if (data_ready_level != 0) {
        QueueLatestDataReady(k_cycle_get_32());
    }
    return 0;
}

/**
 * @brief 将一个传感器寄存器读入 value；返回零或负 SPI errno。
 */
int ImuSensor::ReadRegister(uint8_t reg, uint8_t* value) {
    g_onboard_imu_control_tx[0] = static_cast<uint8_t>(kReadMask | reg);
    g_onboard_imu_control_tx[1] = 0U;
    g_onboard_imu_control_rx[0] = 0U;
    g_onboard_imu_control_rx[1] = 0U;
    const struct spi_buf tx_buf = {
        .buf = g_onboard_imu_control_tx,
        .len = sizeof(g_onboard_imu_control_tx),
    };
    const struct spi_buf rx_buf = {
        .buf = g_onboard_imu_control_rx,
        .len = sizeof(g_onboard_imu_control_rx),
    };
    const struct spi_buf_set tx_set = {.buffers = &tx_buf, .count = 1U};
    const struct spi_buf_set rx_set = {.buffers = &rx_buf, .count = 1U};

    const int rc =
        spi_transceive(spi_dev_, &g_onboard_imu_spi_config, &tx_set, &rx_set);
    if (rc == 0) {
        *value = g_onboard_imu_control_rx[1];
    }
    return rc;
}

/**
 * @brief 写入一个传感器寄存器；返回零或负 SPI errno。
 */
int ImuSensor::WriteRegister(uint8_t reg, uint8_t value) {
    g_onboard_imu_control_tx[0] = static_cast<uint8_t>(reg & ~kReadMask);
    g_onboard_imu_control_tx[1] = value;
    const struct spi_buf tx_buf = {
        .buf = g_onboard_imu_control_tx,
        .len = sizeof(g_onboard_imu_control_tx),
    };
    const struct spi_buf_set tx_set = {.buffers = &tx_buf, .count = 1U};

    return spi_write(spi_dev_, &g_onboard_imu_spi_config, &tx_set);
}

/**
 * @brief 在 GPIO 中断上下文中记录 DRDY 周期时间戳。
 */
void ImuSensor::DataReadyCallback(const struct device* port,
                                  struct gpio_callback* callback,
                                  gpio_port_pins_t pins) {
    ARG_UNUSED(port);
    ARG_UNUSED(pins);
    auto* imu = CONTAINER_OF(callback, ImuSensor, data_ready_callback_);
    imu->QueueLatestDataReady(k_cycle_get_32());
}

/**
 * @brief 无等待替换队列中过时的 DRDY 时间戳。
 */
void ImuSensor::QueueLatestDataReady(uint32_t data_ready_cycle) {
    if (k_msgq_put(&data_ready_queue_, &data_ready_cycle, K_NO_WAIT) != 0) {
        uint32_t obsolete_cycle;
        (void)k_msgq_get(&data_ready_queue_, &obsolete_cycle, K_NO_WAIT);
        (void)k_msgq_put(&data_ready_queue_, &data_ready_cycle, K_NO_WAIT);
    }
}

/**
 * @brief 为选定样本启动异步 DMA 突发传输；返回零或负 errno。
 */
int ImuSensor::StartBurstRead() {
    if (!atomic_cas(&transfer_in_flight_, 0, 1)) {
        return -EBUSY;
    }

    g_onboard_imu_tx[0] = kReadMask | kAccelDataStartRegister;
    memset(&g_onboard_imu_tx[1], 0, kBurstPayloadSize);
    memset(g_onboard_imu_rx, 0, sizeof(g_onboard_imu_rx));
    atomic_clear(&transfer_complete_);
    atomic_clear(&timeout_recovery_started_);
    atomic_set(&transfer_status_, -EINPROGRESS);
    transfer_start_cycle_ = k_cycle_get_32();
    const int rc = spi_transceive_cb(
        spi_dev_, &g_onboard_imu_spi_config, &g_onboard_imu_tx_set,
        &g_onboard_imu_rx_set, TransferCallback, this);
    if (rc != 0) {
        atomic_clear(&transfer_in_flight_);
    }
    return rc;
}

/**
 * @brief 锁存 SPI 完成状态供后续读取，不发布样本。
 */
void ImuSensor::TransferCallback(const struct device* dev, int status,
                                 void* userdata) {
    ARG_UNUSED(dev);
    auto* imu = static_cast<ImuSensor*>(userdata);
    atomic_set(&imu->transfer_status_, status);
    atomic_set(&imu->transfer_complete_, 1);
}

/**
 * @brief 无等待将成功完成的突发数据复制到 burst；仅成功时返回 true。
 */
bool ImuSensor::TryTakeCompleted(Burst& burst) {
    if (!atomic_cas(&transfer_complete_, 1, 0)) {
        return false;
    }
    const int status = atomic_get(&transfer_status_);
    if (status != 0) {
        atomic_clear(&transfer_in_flight_);
        atomic_clear(&timeout_recovery_started_);
        return false;
    }
    memcpy(burst.rx, g_onboard_imu_rx, sizeof(burst.rx));
    burst.data_ready_cycle = active_data_ready_cycle_;
    /* TEMP_H/TEMP_L 位于 0x22/0x23；突发读取从 0x0C 开始。 */
    const int16_t temperature_raw = DecodeBigEndian(&burst.rx[23U]);
    /* HXY 手册公式：T = signed16(raw) / 512 + 23 摄氏度。 */
    burst.temperature_c = 23.0F + static_cast<float>(temperature_raw) / 512.0F;
    atomic_clear(&transfer_in_flight_);
    atomic_clear(&timeout_recovery_started_);
    return true;
}

/**
 * @brief 启动最新排队的 DRDY 突发传输，或恢复超时传输。
 * @return 提交成功返回零；正在传输返回 -EBUSY，无样本返回
 * -EAGAIN，否则返回驱动错误。
 */
int ImuSensor::TryStartAsync() {
    if (atomic_get(&transfer_in_flight_) != 0) {
        const uint32_t elapsed_us =
            k_cyc_to_us_floor32(k_cycle_get_32() - transfer_start_cycle_);
        if (elapsed_us > kTransferTimeoutUs &&
            atomic_cas(&timeout_recovery_started_, 0, 1)) {
            if (atomic_get(&transfer_complete_) != 0) {
                atomic_clear(&timeout_recovery_started_);
                return -EBUSY;
            }
            const int rc = spi_release(spi_dev_, &g_onboard_imu_spi_config);
            ARG_UNUSED(rc);
        }
        return -EBUSY;
    }

    uint32_t data_ready_cycle;
    if (k_msgq_get(&data_ready_queue_, &data_ready_cycle, K_NO_WAIT) != 0) {
        return -EAGAIN;
    }
    active_data_ready_cycle_ = data_ready_cycle;
    return StartBurstRead();
}

}  // namespace modules

#else

namespace modules {

/**
 * @brief 初始化传感器寄存器、SPI 和 DRDY 状态；返回零或负 errno。
 */
int ImuSensor::Init() {
    return -ENOTSUP;
}

/**
 * @brief 无等待将成功完成的突发数据复制到 burst；仅成功时返回 true。
 */
bool ImuSensor::TryTakeCompleted(Burst&) {
    return false;
}

/**
 * @brief 启动最新排队的 DRDY 突发传输，或恢复超时传输。
 * @return 提交成功返回零；正在传输返回 -EBUSY，无样本返回
 * -EAGAIN，否则返回驱动错误。
 */
int ImuSensor::TryStartAsync() {
    return -ENOTSUP;
}

/**
 * @brief 在 GPIO 中断上下文中记录 DRDY 周期时间戳。
 */
void ImuSensor::DataReadyCallback(const struct device*, struct gpio_callback*,
                                  gpio_port_pins_t) {}

/**
 * @brief 锁存 SPI 完成状态供后续读取，不发布样本。
 */
void ImuSensor::TransferCallback(const struct device*, int, void*) {}

/**
 * @brief 配置 SPI、重置传感器寄存器并启用数据就绪 GPIO；返回零或负 errno。
 */
int ImuSensor::InitializeHardware() {
    return -ENOTSUP;
}

/**
 * @brief 将一个传感器寄存器读入 value；返回零或负 SPI errno。
 */
int ImuSensor::ReadRegister(uint8_t, uint8_t*) {
    return -ENOTSUP;
}

/**
 * @brief 写入一个传感器寄存器；返回零或负 SPI errno。
 */
int ImuSensor::WriteRegister(uint8_t, uint8_t) {
    return -ENOTSUP;
}

/**
 * @brief 为选定样本启动异步 DMA 突发传输；返回零或负 errno。
 */
int ImuSensor::StartBurstRead() {
    return -ENOTSUP;
}

}  // namespace modules

#endif

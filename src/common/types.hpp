/* SPDX-License-Identifier: Apache-2.0 */

/* 端口层共享值类型：纯 C++17，不依赖 Zephyr / SSC / 驱动。
 * 所有跨层传递的数据结构都定义在这里，避免 ports 反向依赖 domain。 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace ecat {

enum class Error : uint32_t {
    None = 0,
    Argument,
    Incomplete,
    Capacity,
    Signature,
    Mapping,
    State,
    Wire,
    Sequence,
    Session,
    Lease,
    Transport,
    Protection,
    UartTimeout
};

struct Bytes {
    const uint8_t* data = nullptr;
    size_t size = 0;
};

struct MutableBytes {
    uint8_t* data = nullptr;
    size_t size = 0;
};

/* 主机下发的 CAN 命令帧（总线号由协议字段给出）。 */
struct CanFrame {
    uint8_t bus = 0;
    uint8_t length = 0;
    uint32_t id = 0;
    std::array<uint8_t, 8> data{};
};

/* 硬件接收到的 CAN 原始事件（总线由所属通道隐含）。 */
struct CanSample {
    uint32_t id = 0;
    uint32_t sequence = 0;
    uint32_t capture_us = 0;
    uint8_t length = 0;
    std::array<uint8_t, 8> data{};
};

/* 板级静态资源描述；device 是不透明句柄，由适配器解释。 */
struct CanBusConfig {
    const void* device = nullptr;
    uint8_t index = 0;
    uint32_t bitrate = 0;
};

struct CanFilterConfig {
    uint8_t bus = 0;
    uint32_t id = 0;
    uint32_t mask = 0x7ff;
};

struct UartPortConfig {
    const void* device = nullptr;
    uint8_t index = 0;
    bool configure_8e2 = false;
    bool rx_only = false;
};

/* 硬件中断/DMA 收到的 UART 原始块。 */
struct UartRxBlock {
    uint8_t port = 0;
    uint32_t epoch = 0;
    uint32_t capture_us = 0;
    bool discontinuity = false;
    uint16_t length = 0;
    std::array<uint8_t, 128> data{};
};

/* 端口侧统计（ISR/DMA 更新，线程侧只读快照）。 */
struct CanBusStats {
    uint32_t received = 0;
    uint32_t drops = 0;
    uint32_t completed = 0;
    uint32_t errors = 0;
    bool busy = false;
};

struct UartStats {
    uint32_t received = 0;
    uint32_t drops = 0;
    uint32_t completed = 0;
    uint32_t errors = 0;
    bool busy = false;
    uint32_t queued = 0;
};

struct ImuSample {
    uint32_t sequence = 0;
    uint32_t capture_us = 0;
    float temperature_c = 0;
    std::array<uint8_t, 25> raw{};
};

enum class AlState : uint8_t {
    Init = 0,
    PreOp,
    SafeOp,
    Op,
    Unknown
};

struct StackEvents {
    bool reset_requested = false;
    bool session_stopped = false;
};

/* 板级加热参数（单板分支固定配置）。 */
struct HeaterConfig {
    bool enabled = false;
    float target_c = 45.0f;
    float kp = 0.1f;
    float ki = 0.005f;
    float kd = 0.0f;
    float max_duty = 1.0f;
    uint32_t period_ms = 10;
};

/* SSC 适配器需要的固定 SII 分区/容量参数。 */
struct SscBoardConfig {
    uint16_t rx_max = 0;
    uint16_t tx_max = 0;
    uint32_t ram_end = 0;
    uint32_t rx_address = 0;
    uint32_t tx_address = 0;
};

/* 故障上报钩子：服务只上报，不自行锁存保护。 */
struct FaultSink {
    void* context = nullptr;
    void (*report)(void*, Error) = nullptr;

    void Report(Error reason) const {
        if (report) {
            report(context, reason);
        }
    }
};

/* wire 层语义标志（ABI 5.0 冻结）。 */
constexpr uint16_t kAbsent = 0xffff;
constexpr uint16_t kDataValid = 1;
constexpr uint16_t kAckValid = 2;
constexpr uint16_t kDiscontinuity = 4;
constexpr uint32_t kDiscover = 1;
constexpr uint32_t kArm = 2;
constexpr uint32_t kStop = 4;

}  // namespace ecat

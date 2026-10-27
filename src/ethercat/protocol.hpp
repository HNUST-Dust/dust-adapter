/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace dust::ecat {
constexpr uint32_t kMagic = 0x31435444;
constexpr uint16_t kAbiMajor = 1;
constexpr uint16_t kAbiMinor = 0;
constexpr uint32_t kCycleUs = 1000;  // 默认交换周期；控制器可以独立保持 1 kHz。
constexpr bool ValidCycle(uint32_t us) {
    return us == 125 || us == 250 || us == 500 || us == 1000;
}
constexpr uint32_t kTimeoutUs = 20000;
constexpr uint32_t kProtectionCapability = 1U << 31;
constexpr size_t kMaxImageBytes = 4096;
constexpr size_t kMaxEntries = 1024;
constexpr uint16_t kAbsent = 0xffff;
constexpr uint16_t kDataValid = 1;
constexpr uint16_t kAckValid = 2;
constexpr uint16_t kDiscontinuity = 4;
constexpr uint32_t kDiscover = 1;
constexpr uint32_t kArm = 2;
constexpr uint32_t kStop = 4;

struct Bytes {
    const uint8_t* data = nullptr;
    size_t size = 0;
};
struct MutableBytes {
    uint8_t* data = nullptr;
    size_t size = 0;
};
uint16_t U16(const uint8_t* p);
uint32_t U32(const uint8_t* p);
uint64_t U64(const uint8_t* p);
void Put16(uint8_t* p, uint16_t value);
void Put32(uint8_t* p, uint32_t value);
void Put64(uint8_t* p, uint64_t value);
bool Newer(uint32_t value, uint32_t previous);

struct UartCapacity {
    uint16_t tx_bytes = 0;  // 主机 -> 从站。
    uint16_t rx_bytes = 0;
};
struct Configuration {
    uint32_t signature = 0;
    uint32_t cycle_us = kCycleUs;
    uint32_t watchdog_us = kTimeoutUs;
    std::array<uint8_t, 2> can_tx{};
    std::array<uint8_t, 4> can_rx{};
    std::array<UartCapacity, 6> uart{};
    bool imu = false;
};
// 资源由后续 ESC/板级集成提供，核心不猜测芯片 RAM 或虚构已安装的保护。
struct Limits {
    uint16_t rx_bytes = 0;
    uint16_t tx_bytes = 0;
    uint32_t process_ram_bytes = 0;  // 已扣邮箱/保留区，可用于三缓冲的预算。
    uint8_t can_mask = 0;
    uint8_t uart_tx_mask = 0;
    uint8_t uart_rx_mask = 0;
    bool imu = false;
};
enum class Error : uint32_t {
    None,
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
struct Entry {
    uint16_t index = 0;
    uint8_t subindex = 0;
    uint8_t bits = 0;
    uint16_t offset = 0;
};
struct Mapping {
    uint16_t index = 0;
    uint16_t count = 0;
};
struct ImageLayout {
    uint16_t bytes = 0;
    uint16_t count = 0;
    uint16_t first_pdo = 0;
    uint16_t mapping_count = 0;
    std::array<Mapping, 5> mappings{};
    std::array<Entry, kMaxEntries> entries{};
    uint16_t MappingCount() const;
};
struct Layout {
    ImageLayout rx;
    ImageLayout tx;
    uint32_t signature = 0;
    std::array<uint16_t, 2> source{{kAbsent, kAbsent}};
    std::array<uint16_t, 4> feedback{{kAbsent, kAbsent, kAbsent, kAbsent}};
    std::array<uint16_t, 6> uart_rx{
        {kAbsent, kAbsent, kAbsent, kAbsent, kAbsent, kAbsent}};
    std::array<uint16_t, 6> uart_tx{
        {kAbsent, kAbsent, kAbsent, kAbsent, kAbsent, kAbsent}};
    uint16_t imu = kAbsent;
    uint16_t diagnostics = 0;
};
Error BuildLayout(const Configuration& config, const Limits& limits,
                  Layout& out);
// staging 接收 0x2100:1..22，顺序任意；提交必须同时匹配实际 PDO/SM 映射。
class ConfigurationTransaction {
   public:
    Error Write(uint8_t subindex, uint32_t value);
    Error Commit(const Limits& limits, const ImageLayout& rx,
                 const ImageLayout& tx, Layout& layout) const;
    const Configuration& candidate() const {
        return candidate_;
    }
    void Clear();

   private:
    Configuration candidate_{};
    uint32_t present_ = 0;
};

struct RxHeader {
    uint64_t session = 0;
    uint64_t boot = 0;
    uint32_t exchange = 0;
    uint32_t flags = 0;
    uint32_t signature = 0;
    uint32_t sources = 0;
    uint32_t status_sequence = 0;
    uint8_t status_target = 0;
    uint8_t status_channel = 0;
};
Error DecodeHeader(Bytes image, RxHeader& out);
struct CanFrame {
    uint8_t bus = 0;
    uint8_t length = 0;
    uint32_t id = 0;
    std::array<uint8_t, 8> data{};
};
struct CanBatch {
    bool valid = false;
    uint32_t sequence = 0;
    uint64_t deadline_us = 0;
    uint8_t count = 0;
    std::array<CanFrame, 16> frames{};
};
struct UartDelivery {
    bool valid = false;
    uint32_t sequence = 0;
    bool discontinuity = false;
    Bytes bytes{};
};
// 输出只在本次 Process 成功后有效；UART 视图在调用方改写 Rx 影像前有效。
struct Actions {
    std::array<CanBatch, 2> can{};
    std::array<UartDelivery, 6> uart{};
    uint32_t status_sequence = 0;
    uint8_t status_target = 0;
    uint8_t status_channel = 0;
};
struct Protection {
    void* context = nullptr;
    // ValidateCan 必须只验证，不产生硬件副作用。
    bool (*validate_can)(void*, unsigned source, const CanFrame&) = nullptr;
    void (*stop)(void*, Error) = nullptr;
    bool installed = false;
};
enum class BusinessState : uint32_t {
    Unconfigured = 0,
    Ready = 1,
    Active = 2,
    Protected = 3
};

// 单个执行上下文拥有整个核心；ISR/业务线程通过集成层队列交换数据。
class Core {
   public:
    explicit Core(uint64_t boot_id, Protection protection = {});
    ~Core();
    Core(const Core&) = delete;
    Core& operator=(const Core&) = delete;
    Core(Core&&) = delete;
    Core& operator=(Core&&) = delete;
    Error Configure(const ConfigurationTransaction& transaction,
                    const Limits& limits, const ImageLayout& rx,
                    const ImageLayout& tx);
    Error Process(Bytes image, uint64_t now_us, bool operational,
                  Actions& actions);
    void Tick(uint64_t now_us, bool operational);
    void Protect(Error reason);
    // 在 UART 后端已复制整个块并预留发送空间后调用，才推进累计 ACK。
    bool CommitUart(unsigned port, uint32_t sequence);
    // 上行窗口满返回 false；调用方须累计丢弃并显式标记流中断。
    bool QueueUart(unsigned port, Bytes bytes, uint32_t capture_us,
                   uint64_t now_us, bool discontinuity = false);
    void CompleteStatus(uint32_t sequence, int32_t result,
                        std::array<uint32_t, 3> values);
    BusinessState state() const {
        return state_;
    }
    Error fault() const {
        return fault_;
    }
    const Layout& layout() const {
        return layout_;
    }
    uint64_t session() const {
        return session_;
    }
    uint32_t active_sources() const {
        return source_mask_;
    }
    uint32_t AcceptedCan(unsigned source) const;
    uint32_t UartAck(unsigned port) const;
    bool UartAckValid(unsigned port) const;

    struct CanEvent {
        uint32_t id = 0, sequence = 0, capture_us = 0;
        uint8_t length = 0;
        std::array<uint8_t, 8> data{};
    };
    struct CanFeedback {
        uint32_t sequence = 0, drops = 0;
        const CanEvent* frames = nullptr;
        uint16_t count = 0;
    };
    struct Feedback {
        std::array<CanFeedback, 4> can{};
        std::array<uint8_t, 224> diagnostics{};
        bool imu_valid = false;
        uint32_t imu_sequence = 0, imu_capture_us = 0;
        float temperature = 0;
        std::array<uint8_t, 25> imu_raw{};
    };
    // 显式小端编码；每次成功发布推进 publish_sequence，不分配堆内存。
    Error Publish(MutableBytes image, uint64_t now_us,
                  const Feedback& feedback);

   private:
    struct Source {
        bool present = false;
        uint32_t sequence = 0;
        uint64_t deadline = 0;
        uint32_t digest = 0;
    };
    struct Block {
        uint32_t sequence = 0, capture = 0;
        uint64_t queued_at = 0, sent_at = 0;
        uint16_t length = 0;
        bool sent = false, discontinuity = false;
        std::array<uint8_t, 512> data{};
    };
    struct UartWindow {
        std::array<Block, 4> blocks{};
        unsigned head = 0, count = 0, cursor = 0;
        uint32_t next = 1, last_ack = 0;
        bool have_ack = false;
        bool CanAck(uint32_t value) const;
        void Ack(uint32_t value);
    };
    Configuration config_{};
    Limits limits_{};
    Layout layout_{};
    Protection protection_{};
    BusinessState state_ = BusinessState::Unconfigured;
    Error fault_ = Error::None;
    uint64_t boot_ = 0, session_ = 0, exchange_at_ = 0;
    uint64_t exchange_session_ = 0;
    uint32_t source_mask_ = 0, exchange_ = 0, exchange_digest_ = 0;
    bool have_exchange_ = false;
    std::array<Source, 2> sources_{};
    std::array<uint32_t, 6> uart_ack_{}, uart_offered_{};
    std::array<bool, 6> have_uart_ack_{}, have_uart_offer_{};
    std::array<UartWindow, 6> windows_{};
    uint32_t publish_sequence_ = 0, rejected_ = 0;
    uint32_t status_request_ = 0, status_reply_ = 0;
    int32_t status_result_ = 0;
    std::array<uint32_t, 3> status_values_{};
};
}  // namespace dust::ecat

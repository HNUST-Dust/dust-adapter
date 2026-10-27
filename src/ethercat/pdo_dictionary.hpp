/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include "ethercat/protocol.hpp"

namespace dust::ecat {
// 对应 CoE expedited SDO；完整访问由 SSC 适配层显式拒绝。
// SSC 适配层必须确保只在 PREOP 调用 Write。Freeze 后直到回到 PREOP
// 并 Clear 才能重配；本模块不读取 AL 状态。
class PdoDictionary {
   public:
    Error Write(uint16_t index, uint8_t subindex, uint8_t bytes,
                uint32_t value);
    Error Read(uint16_t index, uint8_t subindex, uint32_t& value) const;
    Error Snapshot(ImageLayout& rx, ImageLayout& tx) const;
    void Freeze() {
        frozen_ = true;
    }
    void Clear();

   private:
    struct Object {
        uint16_t count = 0;
        bool disabled = false;
        std::array<uint32_t, 255> values{};
        std::array<bool, 255> written{};
    };
    std::array<Object, 10> mappings_{};
    std::array<Object, 2> assignments_{};
    bool frozen_ = false;
    Object* Find(uint16_t index);
    const Object* Find(uint16_t index) const;
    Error Image(unsigned direction, ImageLayout& image) const;
};

struct SmRegion {
    uint16_t address = 0;
    uint16_t bytes = 0;  // 单个缓冲区大小；ESC 为过程数据保留三个缓冲区。
};
struct ProcessMemory {
    SmRegion rx;
    SmRegion tx;
    uint32_t end = 0;  // 最后一个三缓冲区之后的地址（不包含）。
};
// begin 已避开邮箱/保留区；end 来自已验证的 ESC RAM 边界，不猜测芯片容量。
// 所有地址按 4 字节对齐。失败时不改写 out。
Error AllocateProcessMemory(uint32_t begin, uint32_t end, uint16_t rx_bytes,
                            uint16_t tx_bytes, ProcessMemory& out);
}  // namespace dust::ecat

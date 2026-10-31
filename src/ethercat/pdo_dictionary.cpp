/* SPDX-License-Identifier: Apache-2.0 */
#include "ethercat/pdo_dictionary.hpp"

namespace dust::ecat {
PdoDictionary::Object* PdoDictionary::Find(uint16_t index) {
    if (index >= 0x1600 && index < 0x1605) {
        return &mappings_[index - 0x1600];
    }
    if (index >= 0x1a00 && index < 0x1a05) {
        return &mappings_[5 + index - 0x1a00];
    }
    if (index == 0x1c12 || index == 0x1c13) {
        return &assignments_[index - 0x1c12];
    }
    return nullptr;
}
const PdoDictionary::Object* PdoDictionary::Find(uint16_t index) const {
    return const_cast<PdoDictionary*>(this)->Find(index);
}
Error PdoDictionary::Write(uint16_t index, uint8_t subindex, uint8_t bytes,
                           uint32_t value) {
    if (frozen_) {
        return Error::State;
    }
    auto* object = Find(index);
    if (!object) {
        return Error::Argument;
    }
    const bool assignment = index == 0x1c12 || index == 0x1c13;
    if (subindex == 0) {
        const unsigned maximum = assignment ? 5 : 255;
        if (bytes != 1 || value > maximum) {
            return Error::Argument;
        }
        if (value == 0) {
            *object = {};
            object->disabled = true;
            return Error::None;
        }
        if (!object->disabled) {
            return Error::State;
        }
        for (unsigned i = 0; i < value; ++i) {
            if (!object->written[i]) {
                return Error::Incomplete;
            }
        }
        object->count = static_cast<uint16_t>(value);
        object->disabled = false;
        return Error::None;
    }
    if (!object->disabled) {
        return Error::State;
    }
    if (assignment) {
        if (bytes != 2 || subindex > 5 || value > 0xffff) {
            return Error::Argument;
        }
        const uint32_t base = index == 0x1c12 ? 0x1600 : 0x1a00;
        if (value < base || value >= base + 5) {
            return Error::Mapping;
        }
    } else {
        if (bytes != 4) {
            return Error::Argument;
        }
        const unsigned bits = value & 0xff;
        const unsigned object_index = value >> 16;
        // 本协议无位填充、无 subindex 0 映射，也不允许映射配置/另一方向对象。
        const unsigned low = index < 0x1a00 ? 0x2000 : 0x3000;
        if (!bits || bits % 8 || !(value & 0xff00) || object_index < low ||
            object_index >= low + 0x100) {
            return Error::Mapping;
        }
    }
    object->values[subindex - 1] = value;
    object->written[subindex - 1] = true;
    return Error::None;
}
Error PdoDictionary::Read(uint16_t index, uint8_t subindex,
                          uint32_t& value) const {
    const auto* object = Find(index);
    if (!object || subindex > object->count) {
        return Error::Argument;
    }
    value = subindex == 0 ? object->count : object->values[subindex - 1];
    return Error::None;
}
Error PdoDictionary::Image(unsigned direction, ImageLayout& image) const {
    image = {};
    const auto& assignment = assignments_[direction];
    if (assignment.disabled || !assignment.count) {
        return Error::Incomplete;
    }
    image.first_pdo = direction ? 0x1a00 : 0x1600;
    image.mapping_count = assignment.count;
    for (unsigned i = 0; i < assignment.count; ++i) {
        const uint16_t index = image.first_pdo + i;
        if (assignment.values[i] != index) {
            return Error::Mapping;
        }
        const auto& mapping = mappings_[direction * 5 + i];
        if (mapping.disabled || !mapping.count) {
            return Error::Incomplete;
        }
        image.mappings[i] = {index, mapping.count};
        for (unsigned j = 0; j < mapping.count; ++j) {
            if (image.count == kMaxEntries) {
                return Error::Capacity;
            }
            const auto value = mapping.values[j];
            const auto bits = static_cast<uint8_t>(value);
            if (image.bytes + bits / 8 > kMaxImageBytes) {
                return Error::Capacity;
            }
            image.entries[image.count++] = {static_cast<uint16_t>(value >> 16),
                                            static_cast<uint8_t>(value >> 8),
                                            bits, image.bytes};
            image.bytes += bits / 8;
        }
    }
    return Error::None;
}
Error PdoDictionary::Snapshot(ImageLayout& rx, ImageLayout& tx) const {
    // 失败时调用方必须丢弃两个输出；禁止将部分结果用于 SAFEOP。
    if (&rx == &tx) {
        return Error::Argument;
    }
    auto result = Image(0, rx);
    if (result != Error::None) {
        return result;
    }
    return Image(1, tx);
}
void PdoDictionary::Clear() {
    for (auto& object : mappings_) {
        object = {};
    }
    for (auto& object : assignments_) {
        object = {};
    }
    frozen_ = false;
}
Error AllocateProcessMemory(uint32_t begin, uint32_t end, uint16_t rx_bytes,
                            uint16_t tx_bytes, ProcessMemory& out) {
    if (begin < 0x1000 || begin >= end || end > 0x10000 || !rx_bytes ||
        !tx_bytes || rx_bytes > kMaxImageBytes || tx_bytes > kMaxImageBytes) {
        return Error::Argument;
    }
    const uint32_t rx = (begin + 3) & ~3U;
    const uint32_t tx = (rx + 3U * rx_bytes + 3) & ~3U;
    const uint32_t used_end = tx + 3U * tx_bytes;
    if (rx > 0xffff || tx > 0xffff || used_end > end) {
        return Error::Capacity;
    }
    out = {{static_cast<uint16_t>(rx), rx_bytes},
           {static_cast<uint16_t>(tx), tx_bytes},
           used_end};
    return Error::None;
}
}  // namespace dust::ecat

/* SPDX-License-Identifier: Apache-2.0 */
#include <cassert>
#include <cstdio>

#include "ethercat/pdo_dictionary.hpp"
using namespace dust::ecat;
int main() {
    PdoDictionary dictionary;
    ImageLayout rx, tx;
    assert(dictionary.Snapshot(rx, tx) == Error::Incomplete);
    assert(dictionary.Snapshot(rx, rx) == Error::Argument);
    assert(dictionary.Write(0x1600, 1, 4, 0x20000120) == Error::State);
    assert(dictionary.Write(0x1600, 0, 2, 0) == Error::Argument);
    assert(dictionary.Write(0x1600, 0, 1, 0) == Error::None);
    assert(dictionary.Write(0x1600, 0, 1, 1) == Error::Incomplete);
    assert(dictionary.Write(0x1600, 1, 4, 0x20000020) == Error::Mapping);
    assert(dictionary.Write(0x1600, 1, 4, 0x30000120) == Error::Mapping);
    assert(dictionary.Write(0x1600, 1, 4, 0x20000107) == Error::Mapping);
    assert(dictionary.Write(0x1600, 1, 4, 0x20000120) == Error::None);
    assert(dictionary.Write(0x1600, 0, 1, 1) == Error::None);
    uint32_t value = 0;
    assert(dictionary.Read(0x1600, 1, value) == Error::None &&
           value == 0x20000120);
    assert(dictionary.Read(0x1600, 2, value) == Error::Argument);
    // 禁用/重配后，旧条目不能被当成新一轮已经写入的条目。
    assert(dictionary.Write(0x1600, 0, 1, 0) == Error::None);
    assert(dictionary.Write(0x1600, 0, 1, 1) == Error::Incomplete);
    assert(dictionary.Write(0x1c12, 0, 1, 0) == Error::None);
    assert(dictionary.Write(0x1c12, 1, 2, 0x1a00) == Error::Mapping);
    assert(dictionary.Write(0x1c12, 6, 2, 0x1600) == Error::Argument);
    assert(dictionary.Write(0x1c12, 0, 1, 6) == Error::Argument);
    dictionary.Freeze();
    assert(dictionary.Write(0x1600, 0, 1, 0) == Error::State);
    dictionary.Clear();
    assert(dictionary.Write(0x1600, 0, 1, 0) == Error::None);
    ProcessMemory memory;
    assert(AllocateProcessMemory(0x1100, 0x3000, 976, 1536, memory) ==
           Error::None);
    assert(memory.rx.address == 0x1100 && memory.tx.address == 0x1c70);
    assert(memory.end == 0x2e70);
    assert(AllocateProcessMemory(0x1100, 0x2e70, 976, 1536, memory) ==
           Error::None);
    assert(AllocateProcessMemory(0x1100, 0x2e6f, 976, 1536, memory) ==
           Error::Capacity);
    assert(AllocateProcessMemory(0x1101, 0x3000, 65, 289, memory) ==
           Error::None);
    assert(memory.rx.address == 0x1104 && memory.tx.address == 0x11c8);
    assert(memory.end == 0x152b);
    const auto end = memory.end;
    assert(AllocateProcessMemory(0x1100, 0x3000, 4096, 4096, memory) ==
           Error::Capacity);
    assert(memory.end == end);
    assert(AllocateProcessMemory(0x1000, 0x10000, 4096, 4096, memory) ==
           Error::None);
    assert(AllocateProcessMemory(0xffff, 0x10000, 1, 1, memory) ==
           Error::Capacity);
    assert(AllocateProcessMemory(0, 0x3000, 64, 288, memory) ==
           Error::Argument);
    assert(AllocateProcessMemory(0x1100, 0x3000, 0, 288, memory) ==
           Error::Argument);
    assert(AllocateProcessMemory(0x1100, 0x10001, 64, 288, memory) ==
           Error::Argument);
    std::puts("PDO staging / SM allocation contracts PASS");
}

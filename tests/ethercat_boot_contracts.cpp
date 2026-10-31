/* SPDX-License-Identifier: Apache-2.0 */
#include <zephyr/drivers/flash.h>
#include <zephyr/fs/nvs.h>

#include <cstdio>
#include <string>

#include "ethercat/io_policy.hpp"
#include "platform.hpp"
using namespace dust::ecat;
namespace dust::ecat {
int AllocateBootIdentity(uint64_t&);
}
static device flash{};
const device* NativeDevice(const char*) {
    return &flash;
}
static std::string scenario;
static std::vector<uint8_t> stored;
static unsigned reads = 0, writes = 0;
extern "C" uint32_t otp_read_from_shadow(uint32_t i) {
    return scenario == "zero_uid" ? 0 : 0x12345678 + i;
}
int flash_get_page_info_by_offs(const device*, size_t offset,
                                flash_pages_info* p) {
    assert(offset == 0xd0000);
    *p = {offset, 4096};
    return 0;
}
int nvs_mount(nvs_fs* fs) {
    assert(fs->sector_count == 2 && fs->sector_size == 4096 &&
           fs->offset == 0xd0000);
    return scenario == "mount_error" ? -EIO : 0;
}
ssize_t nvs_read(nvs_fs*, uint16_t id, void* data, size_t size) {
    assert(id == 1 && size == 16);
    ++reads;
    if (scenario == "read_error") {
        return -EIO;
    }
    if (stored.empty()) {
        return -ENOENT;
    }
    std::memcpy(data, stored.data(), std::min(size, stored.size()));
    if (scenario == "verify_error" && writes) {
        static_cast<uint8_t*>(data)[0] ^= 1;
    }
    return stored.size();
}
ssize_t nvs_write(nvs_fs*, uint16_t id, const void* data, size_t size) {
    assert(id == 1 && size == 16);
    ++writes;
    if (scenario == "write_short") {
        return 8;
    }
    stored.assign(static_cast<const uint8_t*>(data),
                  static_cast<const uint8_t*>(data) + size);
    return scenario == "write_error" ? -EIO : size;
}
int main(int argc, char** argv) {
    assert(argc == 2);
    scenario = argv[1];
    if (scenario == "corrupt") {
        stored.assign(16, 0);
    }
    uint64_t first = 0;
    const int result = AllocateBootIdentity(first);
    if (scenario != "success") {
        assert(result < 0 && first == 0);
        if (scenario == "zero_uid" || scenario == "mount_error" ||
            scenario == "read_error" || scenario == "corrupt") {
            assert(!writes);
        }
        std::puts("Boot identity failure remains closed.");
        return 0;
    }
    assert(result == 0 && first && writes == 1);
    const uint64_t counter = U64(stored.data() + 8);
    uint64_t second = 0;
    assert(AllocateBootIdentity(second) == 0 && second && first != second &&
           U64(stored.data() + 8) > counter);
    Put64(stored.data() + 8, UINT64_MAX);
    uint64_t exhausted = 0;
    assert(AllocateBootIdentity(exhausted) == -EOVERFLOW && exhausted == 0);
    std::puts(
        "Boot identity persistence/readback/exhaustion contracts passed.");
}

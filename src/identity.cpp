/* SPDX-License-Identifier: Apache-2.0 */

/* 启动身份：直接调用 OTP UUID 与 NVS 预留分区。 */

#include "identity.hpp"

#include <cerrno>
#include <cstring>

#include <zephyr/device.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/fs/nvs.h>
#include <zephyr/storage/flash_map.h>

extern "C" {
#include <hpm_otp_drv.h>
#include <hpm_soc.h>
}

#include "domain/protocol/wire.hpp"

namespace ecat {
namespace {

/* counter 必须先持久化并读回，才可把 boot 暴露给主站；零是保留值。 */
bool NextBootIdentity(uint64_t identity, uint64_t previous, uint64_t& counter,
                      uint64_t& boot) {
    if (!identity || previous == UINT64_MAX) {
        return false;
    }
    counter = previous + 1;
    boot = identity ^ counter;
    if (!boot) {
        if (counter == UINT64_MAX) {
            return false;
        }
        boot = identity ^ ++counter;
    }
    return true;
}

}  // namespace

namespace {

constexpr size_t kUidBytes = 16;
constexpr size_t kRecordBytes = 16;
constexpr uint32_t kRecordMagic = 0x31425444U;  // "DTB1"
constexpr uint16_t kRecordId = 1;

nvs_fs storage{};
bool mounted = false;

int MountStorage() {
    storage.flash_device = FIXED_PARTITION_DEVICE(ethercat_eeprom_partition);
    storage.offset = FIXED_PARTITION_OFFSET(ethercat_eeprom_partition);
    if (!device_is_ready(storage.flash_device)) {
        return -ENODEV;
    }
    flash_pages_info page{};
    int result =
        flash_get_page_info_by_offs(storage.flash_device, storage.offset, &page);
    if (result) {
        return result;
    }
    if (page.start_offset != storage.offset || page.size != 4096 ||
        2 * page.size > FIXED_PARTITION_SIZE(ethercat_eeprom_partition)) {
        return -EINVAL;
    }
    storage.sector_size = page.size;
    storage.sector_count = 2;  // 仅占预留区前 8 KiB，不与 SII RAM 镜像共享存储。
    result = nvs_mount(&storage);
    if (result) {
        return result;
    }
    mounted = true;
    return 0;
}

}  // namespace

int AllocateBootIdentity(uint64_t& boot) {
    boot = 0;
    if (!mounted) {
        const int result = MountStorage();
        if (result) {
            return result;
        }
    }

    uint8_t uid[kUidBytes]{};
    for (unsigned i = 0; i < kUidBytes / 4; ++i) {
        const uint32_t word = otp_read_from_shadow(OTP_SOC_UUID_IDX + i);
        std::memcpy(uid + 4 * i, &word, sizeof(word));
    }
    uint64_t identity = 14695981039346656037ULL;
    bool all_zero = true, all_ff = true;
    for (unsigned i = 0; i < kUidBytes; ++i) {
        all_zero &= uid[i] == 0;
        all_ff &= uid[i] == 0xff;
        identity = (identity ^ uid[i]) * 1099511628211ULL;
    }
    if (all_zero || all_ff || !identity) {
        return -ENODATA;
    }

    uint8_t record[kRecordBytes]{};
    const int bytes = nvs_read(&storage, kRecordId, record, sizeof(record));
    const uint32_t tag = uint32_t(identity) ^ uint32_t(identity >> 32);
    uint64_t previous = 0;
    if (bytes != -ENOENT) {
        if (bytes != int(sizeof(record)) || U32(record) != kRecordMagic ||
            U32(record + 4) != tag) {
            return bytes < 0 ? bytes : -EBADMSG;
        }
        previous = U64(record + 8);
    }
    uint64_t counter = 0, candidate = 0;
    if (!NextBootIdentity(identity, previous, counter, candidate)) {
        return -EOVERFLOW;
    }
    Put32(record, kRecordMagic);
    Put32(record + 4, tag);
    Put64(record + 8, counter);
    const int written = nvs_write(&storage, kRecordId, record, sizeof(record));
    if (written != int(sizeof(record))) {
        return written < 0 ? written : -EIO;
    }
    uint8_t verify[kRecordBytes]{};
    if (nvs_read(&storage, kRecordId, verify, sizeof(verify)) !=
            int(sizeof(record)) ||
        std::memcmp(record, verify, sizeof(record)) != 0) {
        return -EIO;
    }
    boot = candidate;
    return 0;
}

}  // namespace ecat

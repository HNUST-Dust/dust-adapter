/* SPDX-License-Identifier: Apache-2.0 */
#include <zephyr/device.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/fs/nvs.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/printk.h>

#include <cerrno>
#include <cstring>

#include "ethercat/io_policy.hpp"
extern "C" {
#include <hpm_otp_drv.h>
#include <hpm_soc.h>
}
namespace dust::ecat {
int AllocateBootIdentity(uint64_t& boot) {
    boot = 0;
    static nvs_fs storage{};
    static bool mounted = false;
    uint64_t identity = 14695981039346656037ULL;
    bool all_zero = true, all_ff = true;
    for (unsigned i = 0; i < OTP_SOC_UUID_LEN / 4; ++i) {
        const uint32_t word = otp_read_from_shadow(OTP_SOC_UUID_IDX + i);
        all_zero &= word == 0;
        all_ff &= word == UINT32_MAX;
        for (unsigned byte = 0; byte < 4; ++byte) {
            identity =
                (identity ^ uint8_t(word >> (8 * byte))) * 1099511628211ULL;
        }
    }
    if (all_zero || all_ff || !identity) {
        return -ENODATA;
    }
    if (!mounted) {
        storage.flash_device =
            FIXED_PARTITION_DEVICE(ethercat_eeprom_partition);
        storage.offset = FIXED_PARTITION_OFFSET(ethercat_eeprom_partition);
        if (!device_is_ready(storage.flash_device)) {
            return -ENODEV;
        }
        flash_pages_info page{};
        int result = flash_get_page_info_by_offs(storage.flash_device,
                                                 storage.offset, &page);
        if (result) {
            return result;
        }
        if (page.start_offset != storage.offset || page.size != 4096 ||
            2 * page.size > FIXED_PARTITION_SIZE(ethercat_eeprom_partition)) {
            return -EINVAL;
        }
        storage.sector_size = page.size;
        storage.sector_count =
            2;  // 仅占预留区前 8 KiB，不与 SII RAM 镜像共享存储。
        printk("Boot NVS: mounting\n");
        result = nvs_mount(&storage);
        printk("Boot NVS: mount=%d\n", result);
        if (result) {
            return result;
        }
        mounted = true;
    }
    uint8_t record[16]{};
    const auto bytes = nvs_read(&storage, 1, record, sizeof(record));
    printk("Boot NVS: record read=%d\n", int(bytes));
    const uint32_t tag = uint32_t(identity) ^ uint32_t(identity >> 32);
    uint64_t previous = 0;
    if (bytes != -ENOENT) {
        if (bytes != sizeof(record) || U32(record) != 0x31425444 ||
            U32(record + 4) != tag) {
            return bytes < 0 ? int(bytes) : -EBADMSG;
        }
        previous = U64(record + 8);
    }
    uint64_t counter = 0, candidate = 0;
    if (!NextBootIdentity(identity, previous, counter, candidate)) {
        return -EOVERFLOW;
    }
    Put32(record, 0x31425444);
    Put32(record + 4, tag);
    Put64(record + 8, counter);
    printk("Boot NVS: committing counter\n");
    const auto written = nvs_write(&storage, 1, record, sizeof(record));
    printk("Boot NVS: commit=%d\n", int(written));
    if (written != sizeof(record)) {
        return written < 0 ? int(written) : -EIO;
    }
    uint8_t verify[16]{};
    if (nvs_read(&storage, 1, verify, sizeof(verify)) != sizeof(verify) ||
        std::memcmp(record, verify, sizeof(record))) {
        return -EIO;
    }
    boot = candidate;
    return 0;
}
}  // namespace dust::ecat

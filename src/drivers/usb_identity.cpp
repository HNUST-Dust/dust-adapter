/* SPDX-License-Identifier: Apache-2.0 */
#include "drivers/usb_identity.hpp"

#include <errno.h>
#if defined(CONFIG_SOC_FAMILY_HPM)
extern "C" {
#include <hpm_otp_drv.h>
#include <hpm_soc.h>
}
#else
#include <zephyr/drivers/hwinfo.h>
#endif

namespace platform {
int ReadUsbIdentity(uint16_t& pid, char* serial, size_t capacity) {
    uint8_t uid[32]{};
    size_t length = 0;
#if defined(CONFIG_SOC_FAMILY_HPM)
    static_assert(OTP_SOC_UUID_LEN <= sizeof(uid));
    length = OTP_SOC_UUID_LEN;
    for (size_t i = 0; i < length / 4; ++i) {
        const uint32_t word = otp_read_from_shadow(OTP_SOC_UUID_IDX + i);
        for (unsigned byte = 0; byte < 4; ++byte) {
            uid[i * 4 + byte] = uint8_t(word >> (byte * 8));
        }
    }
#else
    const auto result = hwinfo_get_device_id(uid, sizeof(uid));
    if (result <= 0) {
        return result < 0 ? int(result) : -ENODATA;
    }
    length = size_t(result);
#endif
    if (!serial || capacity < length * 2 + 1) {
        return -ENOSPC;
    }
    bool all_zero = true;
    bool all_ff = true;
    uint16_t crc = 0xffff;
    constexpr char hex[] = "0123456789ABCDEF";
    for (size_t i = 0; i < length; ++i) {
        all_zero &= uid[i] == 0;
        all_ff &= uid[i] == 0xff;
        serial[i * 2] = hex[uid[i] >> 4];
        serial[i * 2 + 1] = hex[uid[i] & 15];
        // CRC16-CCITT-FALSE：固定 UID 字节顺序保证重启后 PID 不变。
        crc ^= uint16_t(uid[i]) << 8;
        for (unsigned bit = 0; bit < 8; ++bit) {
            crc = uint16_t((crc << 1) ^ ((crc & 0x8000) ? 0x1021 : 0));
        }
    }
    serial[length * 2] = '\0';
    if (all_zero || all_ff) {
        return -ENODATA;
    }
    pid = crc;
    return 0;
}
}  // namespace platform

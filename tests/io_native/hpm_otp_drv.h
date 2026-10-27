/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdint.h>
#define OTP_SOC_UUID_LEN 16
#define OTP_SOC_UUID_IDX 10
uint32_t otp_read_from_shadow(uint32_t);

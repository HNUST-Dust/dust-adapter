/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include "platform.hpp"
#define FIXED_PARTITION_DEVICE(name) NativeDevice("flash")
#define FIXED_PARTITION_OFFSET(name) 0xd0000
#define FIXED_PARTITION_SIZE(name) 0x10000

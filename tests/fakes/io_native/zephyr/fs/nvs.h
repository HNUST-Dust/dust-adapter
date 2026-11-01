/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include "platform.hpp"
#include <sys/types.h>
struct nvs_fs {const device* flash_device{};size_t offset{},sector_size{},sector_count{};};
int nvs_mount(nvs_fs*);
ssize_t nvs_read(nvs_fs*,uint16_t,void*,size_t);
ssize_t nvs_write(nvs_fs*,uint16_t,const void*,size_t);

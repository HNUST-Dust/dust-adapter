/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include "platform.hpp"
struct flash_pages_info { size_t start_offset{}, size{}; };
int flash_get_page_info_by_offs(const device*,size_t,flash_pages_info*);

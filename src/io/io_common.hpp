/* SPDX-License-Identifier: Apache-2.0 */

#ifndef ECAT_IO_COMMON_HPP_
#define ECAT_IO_COMMON_HPP_

#include <cstdint>

#include <zephyr/kernel.h>

namespace ecat {

inline uint64_t Now() {
    return k_ticks_to_us_floor64(k_uptime_ticks());
}

}  // namespace ecat

#endif /* ECAT_IO_COMMON_HPP_ */

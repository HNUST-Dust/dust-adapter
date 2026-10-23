/* SPDX-License-Identifier: Apache-2.0 */
/** @file thread_priorities.hpp
 * @brief Relative Zephyr worker priorities; lower values run first.
 */
#pragma once
namespace dust_adapter::thread_priority {
// Preserve the validated Zephyr transport priorities (smaller is higher).
inline constexpr int kCanTx = 6;
inline constexpr int kUsbRx = 7;
static_assert(kCanTx < kUsbRx);
}  // namespace dust_adapter::thread_priority

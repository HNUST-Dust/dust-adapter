/* SPDX-License-Identifier: Apache-2.0 */

/* gateway 内部共享定义：仅限 gateway_objects.cpp / gateway_ssc.cpp /
 * gateway.cpp 使用。 */

#ifndef DUST_ECAT_GATEWAY_INTERNAL_HPP_
#define DUST_ECAT_GATEWAY_INTERNAL_HPP_

#include <array>
#include <cstring>
#include <new>

#include <zephyr/kernel.h>

#include "protocol/pdo_dictionary.hpp"
#include "gateway_config.h"
#include "ecat/esc_hw.h"
#include "ecat/io_backend.hpp"

extern "C" {
#include "coeappl.h"
#include "ecatappl.h"
#include "ecat_def.h"
#include "ecatslv.h"
#include "objdef.h"
#include "sampleappl.h"
#include "sdoserv.h"
}

namespace dust::ecat {
namespace gw {

#if defined(CONFIG_DUST_ECAT_GATEWAY_DIAG)
#define GW_DIAG(...) printk(__VA_ARGS__)
#else
#define GW_DIAG(...) ((void)0)
#endif

constexpr unsigned kObjectCount = 35;
constexpr unsigned kDiagLimit = 16;

struct Object {
    TOBJECT descriptor{};
    TSDOINFOENTRYDESC* entries = nullptr;
    uint16_t count = 0;  // SSC 在调用自定义回调前直接读取的 subindex 0。
};

/* gateway_objects.cpp */
extern std::array<Object, kObjectCount> objects;
extern std::array<TSDOINFOENTRYDESC, 256> mapping_entries;
extern std::array<TSDOINFOENTRYDESC, 2> assignment_entries;
extern std::array<TSDOINFOENTRYDESC, 2 * kMaxEntries + 22> data_entries;
extern TSDOINFOENTRYDESC empty_entry;
extern unsigned data_entry_count;
extern unsigned object_count;
extern ConfigurationTransaction transaction;
extern PdoDictionary dictionary;
extern Layout prepared;
extern ImageLayout mapped_rx;
extern ImageLayout mapped_tx;
extern Limits limits;
extern std::array<uint32_t, 22> parameters;
bool RegisterDictionaryObjects();
bool ApplyLimits();
bool CheckSm();
void ResetMappingObjects();
bool Describe(const ImageLayout& image, bool tx);

/* gateway_ssc.cpp */
extern bool mapping_ready;
extern bool new_output;
extern volatile bool rearm_pending;
extern unsigned diag_events;

/* gateway.cpp */
extern Core* core;
extern std::array<uint8_t, kMaxImageBytes> rx_image;
extern std::array<uint8_t, kMaxImageBytes> tx_image;
extern Core::Feedback feedback;
extern Actions actions;
extern bool can_dirty;
extern uint64_t last_publish;
extern BusinessState published_state;

inline uint64_t Now() {
    return k_ticks_to_us_floor64(k_uptime_ticks());
}

#if defined(CONFIG_DUST_ECAT_INPUT_LATE_REFRESH)
void RefreshCanBeforeMapping();
#endif

}  // namespace gw
}  // namespace dust::ecat

#endif /* DUST_ECAT_GATEWAY_INTERNAL_HPP_ */

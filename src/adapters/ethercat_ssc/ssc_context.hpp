/* SPDX-License-Identifier: Apache-2.0 */

/* SSC 适配器共享上下文：所有 SSC/C 结构、对象字典表与过程影像集中在这里，
 * 只在本目录内可见；外部模块不认识这些类型。 */
#pragma once

#include <array>
#include <cstdint>

#include "common/abi.hpp"
#include "common/queue.hpp"
#include "common/hi91_compact.hpp"
#include "adapters/ethercat_ssc/io_coordinator.hpp"
#include "adapters/ethercat_ssc/layout_service.hpp"
#include "domain/protection/protection.hpp"
#include "domain/protocol/session.hpp"


extern "C" {
#include "coeappl.h"
#include "ecat_def.h"
#include "ecatappl.h"
#include "ecatslv.h"
#include "objdef.h"
#include "sdoserv.h"
}

namespace ecat {

class SscStack;

struct SscObject {
    TOBJECT descriptor{};
    TSDOINFOENTRYDESC* entries = nullptr;
    uint16_t count = 0;  // SSC 在调用自定义回调前直接读取的 subindex 0。
};

struct SscContext {
    SscStack* stack = nullptr;
    Session* session = nullptr;
    LayoutService* layout = nullptr;
    IoCoordinator* io = nullptr;
    ProtectionService* protection = nullptr;
    const SscBoardConfig* board = nullptr;

    constexpr static unsigned kObjectCount = 37;
    std::array<SscObject, kObjectCount> objects{};
    /* 十个映射对象共享相同元数据；业务记录只为实际布局分配描述。 */
    std::array<TSDOINFOENTRYDESC, 256> mapping_entries{};
    std::array<TSDOINFOENTRYDESC, 2> assignment_entries{};
    std::array<TSDOINFOENTRYDESC, 2 * kMaxEntries + 22> data_entries{};
    TSDOINFOENTRYDESC empty_entry{DEFTYPE_UNSIGNED8, 8, ACCESS_READ};
    unsigned data_entry_count = 0;
    unsigned object_count = 0;
    std::array<uint32_t, 22> parameters{};
    Limits limits{};

    std::array<uint8_t, kMaxImageBytes> rx_image{};
    std::array<uint8_t, kMaxImageBytes> tx_image{};
    Feedback feedback{};
    Actions actions{};
    std::array<uint8_t, 60> device_rx{}, device_tx{};
    std::array<uint8_t, 60> combined_imu{}, combined_uart{};
    struct SbusOffer {
        UartRxBlock block{};
        uint32_t sequence = 0;
    } sbus_offer;
    dust::hi91::Stream hi91_stream;
    std::array<uint8_t, 60> hi91_page{};
    uint32_t hi91_parse_epoch = 0, hi91_session_generation = 0;
    uint32_t hi91_parser_generation = 0;
    uint32_t hi91_sample_seq = 0, hi91_sample_capture = 0;
    uint32_t hi91_device_time_ms = 0;
    bool hi91_sample_valid = false;
    bool hi91_enabled = true, sbus_enabled = true;
    uint32_t device_session = 0;
    uint64_t device_last_rx = 0;
    uint8_t device_state = 1, device_sequence = 0;
    uint8_t gateway_fault_reason = 0;
    bool device_echo = false;
    bool mapping_ready = false;
    bool new_output = false;
    bool can_dirty = false;
    bool stack_ready = false;
    bool reset_requested = false;
    bool session_stopped = false;
    uint64_t last_publish = 0;
    BusinessState published_state = BusinessState::Unconfigured;
};

extern SscContext g_ctx;

bool SscRegisterObjects();
SscObject* SscFind(uint16_t index);
UINT8 SscRead(UINT16 index, UINT8 subindex, UINT32 size, UINT16* data,
              UINT8 complete);
UINT8 SscWrite(UINT16 index, UINT8 subindex, UINT32 size, UINT16* data,
               UINT8 complete);
bool SscDescribe(const ImageLayout& image, bool tx);

}  // namespace ecat

/* SPDX-License-Identifier: Apache-2.0 */
#include <array>
#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "ethercat/protocol.hpp"
#include "gateway.h"
namespace dust::ecat { extern bool native_can_dirty; }
extern "C" {
#include "coeappl.h"
#include "ecat_def.h"
#include "ecatslv.h"
#include "objdef.h"
#define _APPL_INTERFACE_ 1
#include "applInterface.h"
#undef _APPL_INTERFACE_
#include "sampleappl.h"
UINT8 nAlStatus = STATE_PREOP;
BOOL bEcatOutputUpdateRunning = false;
BOOL bEcatInputUpdateRunning = false;
UINT16 EcatWdValue;
UINT16 nPdInputSize;
UINT16 nPdOutputSize;
BOOL bEscIntEnabled;
BOOL bDcSyncActive;
BOOL bEcatFirstOutputsReceived;
void hpm5e_ssc_enter_critical(void) {}
void hpm5e_ssc_leave_critical(void) {}
int printk(const char*, ...) {
    return 0;
}
uint64_t k_cycle_get_64(void) {
    return 42;
}
int64_t k_uptime_ticks(void) {
    return 1000;
}
uint64_t k_ticks_to_us_floor64(uint64_t value) {
    return value;
}
}
extern bool native_io_protected;
extern unsigned native_io_dispatches;
using namespace dust::ecat;
static std::array<uint8_t, 0x3000> esc;
extern "C" void hpm5e_ssc_esc_read(volatile void* out, uint16_t address,
                                   uint16_t count) {
    assert(unsigned(address) + count <= esc.size());
    std::memcpy(const_cast<void*>(out), esc.data() + address, count);
}
static unsigned native_input_refreshes;
extern "C" void PDO_InputMapping(void) {
    ++native_input_refreshes;
    std::array<UINT16, MAX_PD_INPUT_SIZE / 2> mapped{};
    APPL_InputMapping(mapped.data());
}
static uint8_t Write(uint16_t index, uint8_t sub, uint32_t value,
                     unsigned bytes) {
    auto* object = OBJ_GetObjectHandle(index);
    assert(object);
    uint16_t data[2]{};
    Put32(reinterpret_cast<uint8_t*>(data), value);
    return OBJ_Write(index, sub, bytes, object, data, false);
}
static uint32_t ReadScalar(uint16_t index, uint8_t sub) {
    const auto* object = OBJ_GetObjectHandle(index);
    assert(object);
    const auto size = OBJ_GetObjectLength(index, sub, object, false);
    assert(size <= 4);
    uint16_t data[2]{};
    assert(OBJ_Read(index, sub, size, object, data, false) == 0);
    return U32(reinterpret_cast<uint8_t*>(data));
}
static void Map(const ImageLayout& image, uint16_t assignment) {
    assert(Write(assignment, 0, 0, 1) == 0);
    unsigned offset = 0;
    for (unsigned i = 0; i < image.mapping_count; ++i) {
        const auto& mapping = image.mappings[i];
        assert(Write(mapping.index, 0, 0, 1) == 0);
        for (unsigned j = 0; j < mapping.count; ++j) {
            const auto& entry = image.entries[offset++];
            assert(Write(mapping.index, j + 1,
                         (uint32_t(entry.index) << 16) |
                             (uint32_t(entry.subindex) << 8) | entry.bits,
                         4) == 0);
        }
        assert(Write(mapping.index, 0, mapping.count, 1) == 0);
        assert(Write(assignment, i + 1, mapping.index, 2) == 0);
    }
    assert(Write(assignment, 0, image.mapping_count, 1) == 0);
}
static Layout expected;
static void Configure() {
    Configuration c;
    c.can_tx = {1, 0};
    c.can_rx = {8, 8, 8, 8};
    c.uart[0] = {16, 32};
    assert(BuildLayout(c, {1024, 1536, 7936, 15, 0x2f, 63, false}, expected) ==
           Error::None);
    const uint32_t parameters[] = {expected.signature,
                                   1000,
                                   20000,
                                   1,
                                   0,
                                   8,
                                   8,
                                   8,
                                   8,
                                   16,
                                   32,
                                   0,
                                   0,
                                   0,
                                   0,
                                   0,
                                   0,
                                   0,
                                   0,
                                   0,
                                   0,
                                   0};
    // 验证映射与配置 SDO 的到达顺序不影响提交。
    Map(expected.rx, 0x1c12);
    Map(expected.tx, 0x1c13);
    uint16_t input, output;
    assert(APPL_GenerateMapping(&input, &output) != 0);
    for (unsigned sub = 22; sub > 0; --sub) {
        assert(Write(0x2100, sub, parameters[sub - 1], 4) == 0);
    }
    assert(Write(0x2100, 1, expected.signature ^ 1U, 4) == 0);
    assert(APPL_GenerateMapping(&input, &output) != 0 && input == 0 &&
           output == 0);
    assert(Write(0x2100, 1, expected.signature, 4) == 0);
    assert(Write(0x2100, 18, 1, 4) == 0);  // UART5 在 PCB 上只有 RX。
    assert(APPL_GenerateMapping(&input, &output) != 0);
    assert(Write(0x2100, 18, 0, 4) == 0);
    assert(APPL_GenerateMapping(&input, &output) == 0);
    assert(input == expected.tx.bytes && output == expected.rx.bytes);
    // 经由真实 SSC 元数据接口读取逐项位宽、映射标记及业务记录大小。
    for (const auto* image : {&expected.rx, &expected.tx}) {
        for (unsigned i = 0; i < image->count; ++i) {
            const auto& e = image->entries[i];
            const auto* object = OBJ_GetObjectHandle(e.index);
            assert(object);
            const auto* description = OBJ_GetEntryDesc(object, e.subindex);
            assert(description->BitLength == e.bits);
            assert(OBJ_GetObjectLength(e.index, e.subindex, object, false) ==
                   e.bits / 8U);
            assert(description->ObjAccess &
                   (image == &expected.rx ? OBJACCESS_RXPDOMAPPING
                                          : OBJACCESS_TXPDOMAPPING));
        }
    }
    auto* sm = esc.data() + 0x810;
    Put16(sm, DUST_RX_ADDRESS);
    Put16(sm + 2, output);
    sm[4] = 0x64;
    sm[6] = 1;
    Put16(sm + 8, DUST_TX_ADDRESS);
    Put16(sm + 10, input);
    sm[12] = 0x20;
    sm[14] = 1;
    sSyncManOutPar.u16SyncType = SYNCTYPE_FREERUN;
    sSyncManInPar.u16SyncType = SYNCTYPE_FREERUN;
    sm[0] ^= 1;
    assert(APPL_StartInputHandler(nullptr) != 0);
    sm[0] ^= 1;
    sm[4] &= ~0x40;
    assert(APPL_StartInputHandler(nullptr) != 0);
    sm[4] |= 0x40;
    assert(APPL_StartInputHandler(nullptr) == 0);
}
int main() {
    assert(COE_ObjDictionaryInit() == 0);
    esc[6] = 4;
    assert(dust_gateway_init() != 0);
    assert(COE_ObjDictionaryInit() == 0);
    esc[6] = 8;
    assert(dust_gateway_init() == 0);
    assert(!OBJ_GetObjectHandle(0x6000) && !OBJ_GetObjectHandle(0x7010));
    assert(Write(0x1600, 1, 0x20000120, 4) != 0);
    uint16_t data[2]{};
    const auto* configuration = OBJ_GetObjectHandle(0x2100);
    assert(OBJ_Write(0x2100, 1, 4, configuration, data, true) ==
           ABORTIDX_COMPLETE_ACCESS_NOT_SUPPORTED);
    assert(Write(0x2100, 2, 1000, 2) != 0);
    nAlStatus = STATE_SAFEOP;
    assert(Write(0x2100, 2, 1000, 4) != 0);
    nAlStatus = STATE_PREOP;
    Configure();
    assert(Write(0x2100, 2, 1000, 4) != 0);  // 已提交配置不得热改。
    assert(ReadScalar(0x2100, 0) == 22);
    assert(ReadScalar(0x2100, 1) == expected.signature);
    assert(ReadScalar(0x1018, 2) == PRODUCT_CODE);
    assert(ReadScalar(0x1600, 0) == expected.rx.mappings[0].count);
    nAlStatus = STATE_SAFEOP;
    APPL_Application();
    std::array<uint16_t, 2048> tx{};
    APPL_InputMapping(tx.data());
    auto* bytes = reinterpret_cast<uint8_t*>(tx.data());
    assert(U32(bytes) == kMagic &&
           U32(bytes + 40) == uint32_t(BusinessState::Ready));
    assert(!(U32(bytes + 36) & kProtectionCapability));
    assert(ReadScalar(0x3000, 1) == kMagic);
    const auto boot = U64(bytes + 8);
    assert(boot);
    assert(APPL_StartOutputHandler() == 0);
    bEcatOutputUpdateRunning = true;
    nAlStatus = STATE_OP;
    std::array<uint16_t, 2048> rx{};
    auto* request = reinterpret_cast<uint8_t*>(rx.data());
    Put32(request, kMagic);
    Put16(request + 4, 1);
    Put64(request + 8, 99);
    Put32(request + 24, 1);
    Put32(request + 28, kDiscover);
    Put32(request + 32, expected.signature);
    Put32(request + 40, 1000);
    APPL_OutputMapping(rx.data());
    APPL_Application();
    APPL_InputMapping(tx.data());
    assert(U32(bytes + 40) == uint32_t(BusinessState::Ready));
    const auto published = tx;
    for (unsigned i = 0; i < 10; ++i) {
        APPL_Application();
    }
    APPL_InputMapping(tx.data());
    assert(tx == published);  // OP 空转不能推进 UART 发布窗口。
    Put64(request + 16, boot);
    Put32(request + 24, 2);
    Put32(request + 28, kArm);
    APPL_OutputMapping(rx.data());
    APPL_Application();
    APPL_InputMapping(tx.data());
    assert(U32(bytes + 40) == uint32_t(BusinessState::Protected));
    assert(U32(bytes + 44) == uint32_t(Error::Protection));
    assert(!(U32(bytes + 36) & kProtectionCapability));
    nAlStatus = STATE_PREOP;
    APPL_AckErrorInd(0);
    assert(Write(0x2100, 2, 1000, 4) != 0);  // AL 错误确认不清除业务保护。
    assert(APPL_StopInputHandler() == 0);
    assert(COE_ObjDictionaryInit() == 0);
    assert(dust_gateway_init() == 0);
    Configure();
    APPL_Application();
    APPL_InputMapping(tx.data());
    assert(U64(bytes + 8) != boot);
    // 后端声明保护就绪时，真实 SSC 周期允许 Arm 并调用动作派发入口。
    native_io_protected = true;
    assert(COE_ObjDictionaryInit() == 0);
    assert(dust_gateway_init() == 0);
    Configure();
    APPL_Application();
    APPL_InputMapping(tx.data());
    assert(U32(bytes + 36) & kProtectionCapability);
    const auto next_boot = U64(bytes + 8);
    assert(APPL_StartOutputHandler() == 0);
    nAlStatus = STATE_OP;
    Put64(request + 16, next_boot);
    Put32(request + 24, 1);
    const auto dispatched = native_io_dispatches;
    APPL_OutputMapping(rx.data());
    APPL_Application();
    APPL_InputMapping(tx.data());
    assert(dust_gateway_active());
    assert(U32(bytes + 40) == uint32_t(BusinessState::Active));
    assert(native_io_dispatches == dispatched + 1);
    APPL_Application();
    assert(native_io_dispatches == dispatched + 1);
    // A new CAN snapshot publishes without a new output PDO or IO dispatch.
    const auto previous_publication = U32(bytes + 24);
    dust::ecat::native_can_dirty = true;
    APPL_Application();
    APPL_InputMapping(tx.data());
    assert(U32(bytes + 24) == previous_publication + 1);
    assert(native_io_dispatches == dispatched + 1);
    APPL_Application();
    APPL_InputMapping(tx.data());
    assert(U32(bytes + 24) == previous_publication + 1);
    // Main-thread CAN service also performs one SSC input refresh immediately.
    esc[0x440] = 1;
    bEcatInputUpdateRunning = true;
    bEscIntEnabled = false;
    dust::ecat::native_can_dirty = true;
    dust_gateway_poll();
    APPL_InputMapping(tx.data());
    assert(U32(bytes + 24) == previous_publication + 2);
    assert(native_input_refreshes == 1);
    assert(native_io_dispatches == dispatched + 1);
    dust_gateway_poll();
    assert(native_input_refreshes == 1);
    // Synchronized mode / stopped inputs must not get an out-of-cycle write.
    bEscIntEnabled = true;
    dust::ecat::native_can_dirty = true;
    dust_gateway_poll();
    assert(native_input_refreshes == 1);
    bEscIntEnabled = false;
    bEcatInputUpdateRunning = false;
    dust_gateway_poll();
    assert(native_input_refreshes == 1);
    bEcatInputUpdateRunning = true;
    dust_gateway_poll();
    assert(native_input_refreshes == 2);
    // A failed/partial DMA must protect the actual gateway before dispatch.
    extern int native_esc_dma_fault;
    extern unsigned native_io_stops;
    const auto stops_before_dma = native_io_stops;
    native_esc_dma_fault = -5;
    Put32(request + 24, 2);
    APPL_OutputMapping(rx.data());
    APPL_Application();
    assert(!dust_gateway_active());
    assert(native_io_dispatches == dispatched + 1);
    assert(native_io_stops == stops_before_dma + 1);
    dust_gateway_poll();
    assert(native_io_stops == stops_before_dma + 1);
    std::puts("Real SSC SDO / mapping / cycle integration contracts PASS");
}

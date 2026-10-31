/* SPDX-License-Identifier: Apache-2.0 */
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "ethercat/protocol.hpp"
#include "gateway.h"
namespace dust::ecat {
extern bool native_can_dirty, native_can_frame_enabled, native_can_invalid, native_offer_uart;
extern unsigned native_can_checks;
extern Core::CanEvent native_can_frame;
}
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
static bool native_in_isr;
bool k_is_in_isr(void) { return native_in_isr; }
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
static unsigned native_input_refreshes, native_pdram_writes;
static bool native_fail_next_write;
static std::array<UINT16, MAX_PD_INPUT_SIZE / 2> native_mapped_inputs;
extern int native_esc_dma_fault;
extern "C" void hpm5e_ssc_esc_write(const volatile void* data, uint16_t address,
                                    uint16_t count) {
    ++native_pdram_writes;
    if (native_fail_next_write) {
        native_fail_next_write = false;
        native_esc_dma_fault = -5;
        return;
    }
    assert(unsigned(address) + count <= esc.size());
    std::memcpy(esc.data() + address, const_cast<const void*>(data), count);
}
static void WriteMappedInputs() {
    APPL_InputMapping(native_mapped_inputs.data());
    dust_gateway_esc_write_isr(native_mapped_inputs.data(), DUST_TX_ADDRESS, nPdInputSize);
}
extern "C" void PDO_InputMapping(void) {
    ++native_input_refreshes;
    WriteMappedInputs();
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
    nPdInputSize = input;
    nPdOutputSize = output;
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
    // Remaining native gateway contracts explicitly exercise legacy mode too.
    sSyncManOutPar.u16SyncType = sSyncManInPar.u16SyncType = SYNCTYPE_FREERUN;
}
int main() {
    assert(COE_ObjDictionaryInit() == 0);
    esc[6] = 4;
    assert(dust_gateway_init() != 0);
    assert(COE_ObjDictionaryInit() == 0);
    esc[6] = 8;
    assert(dust_gateway_init() == 0);
    // Mirror the standard CoE initialization which this dictionary-only harness
    // otherwise omits. CheckSyncTypeValue also needs configured PDO sizes.
    sSyncManOutPar.subindex0 = sSyncManInPar.subindex0 = 32;
    nPdOutputSize = 96;
    nPdInputSize = 400;
    assert(sSyncManOutPar.u16SyncTypesSupported == SYNCTYPE_FREERUNSUPP);
    assert(Write(0x1c32, 1, SYNCTYPE_SM_SYNCHRON, 2) != 0);
    assert(Write(0x1c32, 1, SYNCTYPE_DCSYNC0, 2) != 0);
    assert(Write(0x1c32, 1, SYNCTYPE_FREERUN, 2) == 0);
    assert(Write(0x1c33, 1, SYNCTYPE_FREERUN, 2) == 0);
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
    assert(bytes[20] == uint32_t(BusinessState::Ready));
    assert(!(U32(bytes + 16) & kProtectionCapability));
    assert(ReadScalar(0x3000, 5) == U32(bytes + 16));
    const auto boot = U32(bytes);
    assert(boot);
    assert(APPL_StartOutputHandler() == 0);
    bEcatOutputUpdateRunning = true;
    nAlStatus = STATE_OP;
    std::array<uint16_t, 2048> rx{};
    auto* request = reinterpret_cast<uint8_t*>(rx.data());
    Put32(request, 99);
    Put32(request + 8, 1);
    request[12] = uint8_t(kDiscover);
    APPL_OutputMapping(rx.data());
    APPL_Application();
    APPL_InputMapping(tx.data());
    assert(bytes[20] == uint32_t(BusinessState::Ready));
    const auto published = tx;
    for (unsigned i = 0; i < 10; ++i) {
        APPL_Application();
    }
    APPL_InputMapping(tx.data());
    assert(tx == published);  // OP 空转不能推进 UART 发布窗口。
    Put32(request + 4, boot);
    Put32(request + 8, 2);
    request[12] = uint8_t(kArm);
    APPL_OutputMapping(rx.data());
    APPL_Application();
    APPL_InputMapping(tx.data());
    assert(bytes[20] == uint32_t(BusinessState::Protected));
    assert(bytes[21] == uint32_t(Error::Protection));
    assert(!(U32(bytes + 16) & kProtectionCapability));
    nAlStatus = STATE_PREOP;
    APPL_AckErrorInd(0);
    assert(Write(0x2100, 2, 1000, 4) != 0);  // AL 错误确认不清除业务保护。
    assert(APPL_StopInputHandler() == 0);
    assert(COE_ObjDictionaryInit() == 0);
    assert(dust_gateway_init() == 0);
    Configure();
    APPL_Application();
    APPL_InputMapping(tx.data());
    assert(U32(bytes) != boot);
    // 后端声明保护就绪时，真实 SSC 周期允许 Arm 并调用动作派发入口。
    native_io_protected = true;
    assert(COE_ObjDictionaryInit() == 0);
    assert(dust_gateway_init() == 0);
    Configure();
    APPL_Application();
    APPL_InputMapping(tx.data());
    assert(U32(bytes + 16) & kProtectionCapability);
    const auto next_boot = U32(bytes);
    assert(APPL_StartOutputHandler() == 0);
    nAlStatus = STATE_OP;
    Put32(request + 4, next_boot);
    Put32(request + 8, 1);
    const auto dispatched = native_io_dispatches;
    APPL_OutputMapping(rx.data());
    APPL_Application();
    APPL_InputMapping(tx.data());
    assert(dust_gateway_active());
    assert(bytes[20] == uint32_t(BusinessState::Active));
    assert(native_io_dispatches == dispatched + 1);
    APPL_Application();
    assert(native_io_dispatches == dispatched + 1);
    // A new CAN snapshot publishes without a new output PDO or IO dispatch.
    const auto previous_publication = U32(bytes + 8);
    dust::ecat::native_can_dirty = true;
    APPL_Application();
    APPL_InputMapping(tx.data());
    assert(U32(bytes + 8) == previous_publication + 1);
    assert(native_io_dispatches == dispatched + 1);
    APPL_Application();
    APPL_InputMapping(tx.data());
    assert(U32(bytes + 8) == previous_publication + 1);
    // Main-thread CAN service also performs one SSC input refresh immediately.
    esc[0x440] = 1;
    bEcatInputUpdateRunning = true;
    bEscIntEnabled = false;
    // Complete new publications once; suppress only repeated commits in Active FreeRun.
    unsigned writes = native_pdram_writes;
    WriteMappedInputs();
    assert(native_pdram_writes == writes + 1);
    WriteMappedInputs();
#if defined(CONFIG_DUST_ECAT_INPUT_WRITE_COALESCE)
    assert(native_pdram_writes == writes + 1);

#else
    assert(native_pdram_writes == writes + 2);
#endif
    // Even a rejected lifecycle transition revokes a previous commit credit.
    writes = native_pdram_writes;
    assert(APPL_StartOutputHandler() != 0); // Already Active, not Ready.
    WriteMappedInputs();
    assert(native_pdram_writes == writes + 1);
    // Sync/DC/ISR/stopped-input calls must execute normally, and invalidate credit.
    for (unsigned mode = 0; mode < 4; ++mode) {
        bEscIntEnabled = mode == 0;
        bDcSyncActive = mode == 1;
        native_in_isr = mode == 2;
        bEcatInputUpdateRunning = mode != 3;
        writes = native_pdram_writes;
        WriteMappedInputs(); WriteMappedInputs();
        assert(native_pdram_writes == writes + 2);
        bEscIntEnabled = bDcSyncActive = native_in_isr = false;
        bEcatInputUpdateRunning = true;
        WriteMappedInputs();
        assert(native_pdram_writes == writes + 3);
    }
    // An interrupted mapping handshake or partial/non-PDO write is never coalesced.
    writes = native_pdram_writes;
    APPL_InputMapping(native_mapped_inputs.data());
    dust_gateway_esc_write_isr(native_mapped_inputs.data(), DUST_TX_ADDRESS, 4);
    dust_gateway_esc_write_isr(native_mapped_inputs.data(), DUST_TX_ADDRESS, 4);
    dust_gateway_esc_write_isr(native_mapped_inputs.data(), 0x1000, 4);
    assert(native_pdram_writes == writes + 3);
    WriteMappedInputs();
    // A different source pointer does not match the mapping handoff.
    writes = native_pdram_writes;
    APPL_InputMapping(native_mapped_inputs.data());
    dust_gateway_esc_write_isr(tx.data(), DUST_TX_ADDRESS, nPdInputSize);
    dust_gateway_esc_write_isr(tx.data(), DUST_TX_ADDRESS, nPdInputSize);
    assert(native_pdram_writes == writes + 2);
    // A failed transport write cannot mark a publication as committed.
    writes = native_pdram_writes;
    bEscIntEnabled = true; // Force a write even if this revision already committed.
    native_fail_next_write = true;
    WriteMappedInputs();
    bEscIntEnabled = false;
    native_esc_dma_fault = 0;
    WriteMappedInputs();
    assert(native_pdram_writes == writes + 2);
    // New CAN publication must reach ESC, then idle passes leave bytes/version unchanged.
    writes = native_pdram_writes;
    dust::ecat::native_can_dirty = true;
    APPL_Application();
    WriteMappedInputs();
    assert(native_pdram_writes == writes + 1);
    const auto committed_esc = esc;
    WriteMappedInputs();
#if defined(CONFIG_DUST_ECAT_INPUT_WRITE_COALESCE)
    assert(native_pdram_writes == writes + 1);
    assert(esc == committed_esc);

#else
    assert(native_pdram_writes == writes + 2);
    assert(esc == committed_esc);
#endif
    // Account for the extra publication made above in the existing sequence checks.
    dust::ecat::native_can_dirty = true;
    dust_gateway_poll();
    APPL_InputMapping(tx.data());
    assert(U32(bytes + 8) == previous_publication + 3);
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
    Put32(request + 8, 2);
    APPL_OutputMapping(rx.data());
    APPL_Application();
    assert(!dust_gateway_active());
    assert(native_io_dispatches == dispatched + 1);
    assert(native_io_stops == stops_before_dma + 1);
    dust_gateway_poll();
    assert(native_io_stops == stops_before_dma + 1);
    native_esc_dma_fault = 0;
    APPL_Application();
    writes = native_pdram_writes;
    WriteMappedInputs(); WriteMappedInputs();
    assert(native_pdram_writes == writes + 2);
    assert(esc[DUST_TX_ADDRESS + 20] == uint32_t(BusinessState::Protected));
    assert(APPL_StopInputHandler() == 0);
    APPL_InputMapping(native_mapped_inputs.data());
    writes = native_pdram_writes;
    dust_gateway_esc_write_isr(native_mapped_inputs.data(), DUST_TX_ADDRESS, nPdInputSize);
    dust_gateway_esc_write_isr(native_mapped_inputs.data(), DUST_TX_ADDRESS, nPdInputSize);
    assert(native_pdram_writes == writes + 2);
    auto ArmFreshGateway = [&] {
        native_can_dirty = native_can_frame_enabled = native_can_invalid = native_offer_uart = false;
        bEcatOutputUpdateRunning = bEcatInputUpdateRunning = false;
        nAlStatus = STATE_PREOP;
        assert(COE_ObjDictionaryInit() == 0);
        assert(dust_gateway_init() == 0);
        Configure();
        APPL_Application();
        APPL_InputMapping(tx.data());
        Put32(request + 4, U32(bytes));
        Put32(request + 8, 1);
        request[12] = uint8_t(kArm);
        assert(APPL_StartOutputHandler() == 0);
        bEcatOutputUpdateRunning = bEcatInputUpdateRunning = true;
        nAlStatus = STATE_OP;
        esc[0x440] = 1;
        APPL_OutputMapping(rx.data());
        APPL_Application();
        assert(dust_gateway_active());
    };
    ArmFreshGateway();
    WriteMappedInputs(); WriteMappedInputs();
#if defined(CONFIG_DUST_ECAT_INPUT_LATE_REFRESH)
    // Publish a real UART block, then deliver CAN after the normal application snapshot.
    native_offer_uart = true;
    Put32(request + 8, 2);
    APPL_OutputMapping(rx.data());
    APPL_Application();
    APPL_InputMapping(tx.data());
    const unsigned uart_offset = expected.uart_tx[0];
    assert(U16(bytes + uart_offset + 12) == 4);
    std::array<uint8_t, 48> uart_before;
    std::copy_n(bytes + uart_offset, uart_before.size(), uart_before.begin());
    const auto before_late_publication = U32(bytes + 8);
    const auto before_late_dispatches = native_io_dispatches;
    const auto before_late_mappings = native_input_refreshes;
    native_can_frame = {};
    native_can_frame.id = 0x13; native_can_frame.sequence = 77;
    native_can_frame.capture_us = 1000; native_can_frame.length = 8;
    native_can_frame.data[0] = 3;
    native_can_frame_enabled = native_can_dirty = true;
    writes = native_pdram_writes;
    WriteMappedInputs();
    const auto* committed = esc.data() + DUST_TX_ADDRESS;
    assert(U32(committed + 8) == before_late_publication + 1);
    assert(committed[expected.feedback[3] + 8] == 1);
    assert(U32(committed + expected.feedback[3] + 11) == 77);
    assert(std::equal(uart_before.begin(), uart_before.end(), committed + uart_offset));
    assert(native_io_dispatches == before_late_dispatches);
    assert(native_input_refreshes == before_late_mappings); // No recursive mapping.
    assert(native_pdram_writes == writes + 1); // Replaces, never adds, an ESC write.

    WriteMappedInputs();

    assert(U32(esc.data() + DUST_TX_ADDRESS + 8) == before_late_publication + 1);
    for (unsigned mode = 0; mode < 5; ++mode) {
        bEscIntEnabled = mode == 0;
        bDcSyncActive = mode == 1;
        native_in_isr = mode == 2;
        bEcatInputUpdateRunning = mode != 3;
        bEcatOutputUpdateRunning = mode != 4;
        native_can_dirty = true;
        const auto checks = native_can_checks;
        APPL_InputMapping(tx.data());
        assert(native_can_checks == checks && native_can_dirty);
        bEscIntEnabled = bDcSyncActive = native_in_isr = false;
        bEcatInputUpdateRunning = bEcatOutputUpdateRunning = true;
        native_can_dirty = false;
    }
    // Do not consume late feedback against an output image still awaiting Process.
    APPL_OutputMapping(rx.data());
    native_can_dirty = true;
    const auto checks = native_can_checks;
    APPL_InputMapping(tx.data());
    assert(native_can_checks == checks && native_can_dirty);
    APPL_Application();
    WriteMappedInputs();
#endif
    // A late checkpoint must not replace the independent watchdog poll.
    const auto stops_before_watchdog = native_io_stops;
    esc[0x440] = 0;
    dust_gateway_poll();
    assert(!dust_gateway_active());
    assert(native_io_stops == stops_before_watchdog + 1);
    APPL_Application();
    WriteMappedInputs();
    assert(esc[DUST_TX_ADDRESS + 20] == uint32_t(BusinessState::Protected));
#if defined(CONFIG_DUST_ECAT_INPUT_LATE_REFRESH)
    ArmFreshGateway();
    WriteMappedInputs();
    native_can_frame_enabled = native_can_invalid = native_can_dirty = true;
    const auto stops_before_invalid_snapshot = native_io_stops;
    writes = native_pdram_writes;
    WriteMappedInputs();
    assert(!dust_gateway_active());
    assert(native_io_stops == stops_before_invalid_snapshot + 1);
    assert(native_pdram_writes == writes + 1);
    assert(U32(esc.data() + DUST_TX_ADDRESS) != 0);
    assert(esc[DUST_TX_ADDRESS + 20] == uint32_t(BusinessState::Protected));
    assert(esc.data()[DUST_TX_ADDRESS + expected.feedback[3] + 8] == 0);

    native_can_frame_enabled = native_can_invalid = native_can_dirty = false;
#endif
    std::puts("Real SSC SDO / mapping / cycle integration contracts PASS");
}

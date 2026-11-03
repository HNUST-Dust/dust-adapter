/* SPDX-License-Identifier: Apache-2.0 */

/* SSC 栈适配器：生命周期、回调翻译、发布调度与状态路由。
 * 本文件是唯一把 SSC 全局状态/ESC 寄存器与 domain 服务连接起来的地方。 */

#include "adapters/ethercat_ssc/ssc_stack.hpp"

#include "adapters/ethercat_ssc/ssc_context.hpp"

#include <cstring>

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

/* ESC 生命周期/寄存器原语由 adapters/esc 提供；这里只声明用到的入口。 */
extern "C" {
uint16_t MainInit(void);
void MainLoop(void);
void PDI_Isr(void);
void esc_set_pdi_hook(void (*hook)(void));
void esc_set_reset_hook(void (*hook)(void));
void esc_start(void);
void esc_stop(void);
void esc_update_link(void);
void esc_read(volatile void *data, uint16_t address, uint16_t length);
}
#include "common/abi.hpp"
#include "domain/protocol/wire.hpp"

namespace ecat {

SscContext g_ctx;

namespace {

uint64_t NowUs() {
    return k_ticks_to_us_floor64(k_uptime_ticks());
}

void PdiHook() {
    if (g_ctx.stack_ready) {
        PDI_Isr();
    }
}

void ResetHook() {
    g_ctx.reset_requested = true;
}

bool RefreshGuard() {
    return g_ctx.session && g_ctx.session->state() == BusinessState::Active &&
           !g_ctx.new_output && bEcatInputUpdateRunning &&
           bEcatOutputUpdateRunning && !bEscIntEnabled && !bDcSyncActive &&
           nAlStatus == STATE_OP && !k_is_in_isr();
}

}  // namespace

SscStack::SscStack(Session& session, LayoutService& layout, IoCoordinator& io,
                   ProtectionService& protection,
                   const SscBoardConfig& board)
    : session_(session),
      layout_(layout),
      io_(io),
      protection_(protection),
      board_(board) {
    g_ctx.stack = this;
    g_ctx.session = &session_;
    g_ctx.layout = &layout_;
    g_ctx.io = &io_;
    g_ctx.protection = &protection_;
    g_ctx.board = &board_;
}

int SscStack::Start() {
    esc_set_pdi_hook(PdiHook);
    esc_set_reset_hook(ResetHook);
    if (MainInit() != 0U) {
        return -1;
    }
    ResetRuntimeState();
    /* 启动/ESC 复位后 ECAT_Init() 重建字典；同一会话内的 rearm 只重建状态，
     * 必须保留已注册的自定义对象。 */
    if (OBJ_GetObjectHandle(0x2100) == nullptr && !SscRegisterObjects()) {
        return -1;
    }
    return ApplyLimits();
}

void SscStack::Enable() {
    g_ctx.stack_ready = true;
    esc_start();
}

void SscStack::Stop() {
    g_ctx.stack_ready = false;
    esc_stop();
}

void SscStack::Step() {
    MainLoop();
}

StackEvents SscStack::TakeEvents() {
    StackEvents events;
    events.reset_requested = g_ctx.reset_requested;
    events.session_stopped = g_ctx.session_stopped;
    g_ctx.reset_requested = false;
    g_ctx.session_stopped = false;
    return events;
}

AlState SscStack::Al() const {
    switch (nAlStatus & STATE_MASK) {
        case STATE_INIT:
            return AlState::Init;
        case STATE_PREOP:
            return AlState::PreOp;
        case STATE_SAFEOP:
            return AlState::SafeOp;
        case STATE_OP:
            return AlState::Op;
        default:
            return AlState::Unknown;
    }
}

bool SscStack::Active() const {
    return session_.state() == BusinessState::Active;
}

bool SscStack::TransportHealthy() const {
    return transport_healthy_;
}

void SscStack::Poll() {
    if (!g_ctx.session) {
        return;
    }
    uint8_t watchdog = 0;
    esc_read(&watchdog, 0x0440, 1);
    const uint64_t now = NowUs();
    transport_healthy_ = bEcatOutputUpdateRunning && (watchdog & 1) != 0;
    session_.Tick(now, transport_healthy_);
    io_.Service(session_, now);

    if (session_.state() == BusinessState::Active) {
        g_ctx.can_dirty |= io_.DrainCan(NowUs());
        if (g_ctx.can_dirty && g_ctx.mapping_ready &&
            bEcatInputUpdateRunning && !bEscIntEnabled) {
            PublishCanOnly(now);
            if (g_ctx.can_dirty == false) {
                PDO_InputMapping();
            }
        }
    }

    static int64_t next_link_poll = 0;
    const int64_t now_ms = k_uptime_get();
    if (now_ms >= next_link_poll) {
        esc_update_link();
        next_link_poll = now_ms + 100;
    }
}

uint8_t SscStack::ReadObject(uint16_t index, uint8_t subindex,
                             uint32_t size, uint16_t* data, uint8_t complete) {
    return SscRead(index, subindex, size, data, complete);
}

uint8_t SscStack::WriteObject(uint16_t index, uint8_t subindex,
                              uint32_t size, uint16_t* data,
                              uint8_t complete) {
    return SscWrite(index, subindex, size, data, complete);
}

uint16_t SscStack::GenerateMapping(uint16_t* input, uint16_t* output) {
    *input = 0;
    *output = 0;
    g_ctx.mapping_ready = false;
    const Error commit = layout_.GenerateMapping(g_ctx.limits);
    if (commit != Error::None ||
        !io_.CanCapacityValid(layout_.transaction().candidate())) {
        printk("Gateway: mapping rejected: commit=%u\n", unsigned(commit));
        return ALSTATUSCODE_INVALIDOUTPUTMAPPING;
    }
    g_ctx.data_entry_count = 0;
    for (unsigned i = 0; i < g_ctx.object_count; ++i) {
        auto& object = g_ctx.objects[i];
        if (object.descriptor.Index >= 0x2000 &&
            object.descriptor.Index != 0x2100) {
            object.count = 0;
            object.descriptor.ObjDesc.ObjFlags = OBJCODE_REC << 8;
            object.descriptor.pEntryDesc = &g_ctx.empty_entry;
        }
    }
    if (!SscDescribe(layout_.prepared().rx, false) ||
        !SscDescribe(layout_.prepared().tx, true)) {
        return ALSTATUSCODE_INVALIDOUTPUTMAPPING;
    }
    *input = layout_.prepared().tx.bytes;
    *output = layout_.prepared().rx.bytes;
    g_ctx.mapping_ready = true;
    return ALSTATUSCODE_NOERROR;
}

uint16_t SscStack::StartInput(uint16_t*) {
    const bool freerun = sSyncManOutPar.u16SyncType == SYNCTYPE_FREERUN &&
                         sSyncManInPar.u16SyncType == SYNCTYPE_FREERUN;
    if (!g_ctx.mapping_ready) {
        printk("Gateway: SAFEOP rejected: mapping not ready\n");
        return ALSTATUSCODE_INVALIDOUTPUTMAPPING;
    }
    uint8_t sm[16]{};
    esc_read(sm, 0x810, sizeof(sm));
    const bool sm_ok =
        U16(sm) == board_.rx_address &&
        U16(sm + 2) == layout_.prepared().rx.bytes &&
        (sm[4] & 0x47) == 0x44 && (sm[6] & 1) &&
        U16(sm + 8) == board_.tx_address &&
        U16(sm + 10) == layout_.prepared().tx.bytes && (sm[12] & 0x07) == 0 &&
        (sm[14] & 1);
    if (!sm_ok || !freerun) {
        printk("Gateway: SAFEOP rejected: SM/sync mismatch\n");
        return ALSTATUSCODE_INVALIDOUTPUTMAPPING;
    }
    const Error configured = session_.Configure(
        layout_.transaction(), g_ctx.limits, layout_.mapped_rx(),
        layout_.mapped_tx());
    if (configured != Error::None) {
        printk("Gateway: SAFEOP rejected: configure error %u\n",
               unsigned(configured));
        return ALSTATUSCODE_INVALIDOUTPUTMAPPING;
    }
    io_.Configure(layout_.transaction().candidate());
    layout_.Freeze();
    printk("Gateway: SAFEOP accepted (Rx=%u Tx=%u)\n",
           layout_.prepared().rx.bytes, layout_.prepared().tx.bytes);
    return ALSTATUSCODE_NOERROR;
}

uint16_t SscStack::StopInput() {
    if (g_ctx.session &&
        g_ctx.session->state() != BusinessState::Unconfigured) {
        protection_.Report(Error::Transport);
        g_ctx.session_stopped = true;
    }
    g_ctx.mapping_ready = false;
    g_ctx.new_output = false;
    g_ctx.rx_image.fill(0);
    g_ctx.tx_image.fill(0);
    return ALSTATUSCODE_NOERROR;
}

uint16_t SscStack::StartOutput() {
    const bool ok = g_ctx.mapping_ready && g_ctx.session &&
                    g_ctx.session->state() == BusinessState::Ready;
    if (!ok) {
        printk("Gateway: OP rejected: mapping=%u\n",
               unsigned(g_ctx.mapping_ready));
    }
    return ok ? ALSTATUSCODE_NOERROR : ALSTATUSCODE_INVALIDOUTPUTMAPPING;
}

uint16_t SscStack::StopOutput() {
    if (g_ctx.session &&
        g_ctx.session->state() == BusinessState::Active) {
        protection_.Report(Error::Transport);
    }
    g_ctx.new_output = false;
    return ALSTATUSCODE_NOERROR;
}

void SscStack::OutputMapping(uint16_t* data) {
    if (!g_ctx.mapping_ready) {
        return;
    }
    std::memcpy(g_ctx.rx_image.data(), data, layout_.prepared().rx.bytes);
    g_ctx.new_output = true;
}

void SscStack::InputMapping(uint16_t* data) {
    if (!g_ctx.mapping_ready) {
        return;
    }
#if defined(CONFIG_ECAT_INPUT_LATE_REFRESH)
    RefreshCanBeforeMapping();
#endif
    std::memcpy(data, g_ctx.tx_image.data(), layout_.prepared().tx.bytes);
}

void SscStack::RefreshCanBeforeMapping() {
    if (!RefreshGuard() || !g_ctx.mapping_ready) {
        return;
    }
    g_ctx.can_dirty |= io_.DrainCan(NowUs());
    if (!g_ctx.can_dirty) {
        return;
    }
    PublishCanOnly(NowUs());
}

void SscStack::Application() {
    if (!g_ctx.mapping_ready || !g_ctx.session ||
        g_ctx.session->state() == BusinessState::Unconfigured) {
        return;
    }
    const uint64_t now = NowUs();
    session_.Tick(now, bEcatOutputUpdateRunning);
    if (session_.state() == BusinessState::Active) {
        g_ctx.can_dirty |= io_.DrainCan(NowUs());
    }

    const BusinessState before = session_.state();
    const bool received = g_ctx.new_output;
    if (received) {
        g_ctx.new_output = false;
        const Error processed =
            session_.Process({g_ctx.rx_image.data(), layout_.prepared().rx.bytes},
                             now, bEcatOutputUpdateRunning, g_ctx.actions);
        if (processed == Error::None) {
            HandleStatusRequest();
            if (!io_.Dispatch(session_, g_ctx.actions, now)) {
                printk("Gateway: dispatch rejected\n");
            }
        } else {
            printk("Gateway: RxPDO rejected: err=%u\n", unsigned(processed));
        }
    }
    if (before != BusinessState::Active &&
        session_.state() == BusinessState::Active) {
        io_.EnterActive(session_, now);
    }
    io_.Service(session_, now);

    /* 常规发布推进 UART 窗口；额外 CAN 发布保留上一份 UART 字节及窗口状态。 */
    const bool changed = g_ctx.published_state != session_.state();
    const bool regular =
        received || changed ||
        (!bEcatOutputUpdateRunning &&
         now - g_ctx.last_publish >=
             layout_.transaction().candidate().cycle_us);
    if (regular) {
        PublishRegular(now);
    } else if (g_ctx.can_dirty) {
        PublishCanOnly(now);
    }
}

void SscStack::PublishRegular(uint64_t now) {
    io_.Capture(session_, g_ctx.feedback, now);
    const Error published =
        session_.Publish({g_ctx.tx_image.data(), layout_.prepared().tx.bytes},
                         now, g_ctx.feedback, true);
    if (published == Error::None) {
        g_ctx.can_dirty = false;
    } else {
        printk("Gateway: publish failed: err=%u\n", unsigned(published));
    }
    g_ctx.last_publish = now;
    g_ctx.published_state = session_.state();
}

void SscStack::PublishCanOnly(uint64_t now) {
    g_ctx.feedback = {};
    io_.CaptureCan(g_ctx.feedback);
    const Error published =
        session_.Publish({g_ctx.tx_image.data(), layout_.prepared().tx.bytes},
                         now, g_ctx.feedback, false);
    if (published == Error::None) {
        g_ctx.can_dirty = false;
    } else {
        protection_.Report(published);
        g_ctx.feedback = {};
        PublishRegular(now);
    }
    g_ctx.last_publish = now;
    g_ctx.published_state = session_.state();
}

void SscStack::HandleStatusRequest() {
    if (!g_ctx.actions.status_sequence) {
        return;
    }
    const unsigned target = g_ctx.actions.status_target & 3;
    const unsigned page = g_ctx.actions.status_target >> 2;
    const unsigned channel = g_ctx.actions.status_channel;
    std::array<uint32_t, 3> values{};
    int result = -EINVAL;
    if (target == 0 && channel == 0) {
        if (!page) {
            values = {uint32_t(session_.state()),
                      protection_.ProtectionSignature(),
                      protection_.CommandTimeoutUs()};
            result = 0;
        } else if (page == 1) {
            values = {protection_.ErrorCount(), uint32_t(io_.Initialized()),
                      uint32_t(protection_.Latched())};
            result = 0;
        } else {
            result = -ENOTSUP;
        }
    } else if (target == 1 && channel < 4) {
        result = io_.QueryCanStatus(channel, page, values);
    } else if (target == 2 && channel >= 1 && channel <= 6) {
        result = io_.QueryUartStatus(channel - 1, page, values);
    }
    session_.CompleteStatus(g_ctx.actions.status_sequence, int32_t(result),
                            values);
}

void SscStack::AckError() {}

int SscStack::ApplyLimits() {
    uint8_t ram_kib = 0;
    esc_read(&ram_kib, 0x0006, 1);
    const uint32_t ram_end = 0x1000U + uint32_t(ram_kib) * 1024U;
    if (ram_end < board_.ram_end) {
        printk("Gateway: ESC RAM %u KiB cannot fit SII partitions\n", ram_kib);
        return -1;
    }
    g_ctx.limits = {board_.rx_max, board_.tx_max,
                    board_.ram_end - board_.rx_address, 0, 0, 0, false};
    io_.ApplyLimits(g_ctx.limits);
    if (board_.rx_address + 3U * g_ctx.limits.rx_bytes > board_.tx_address ||
        board_.tx_address + 3U * g_ctx.limits.tx_bytes > ram_end) {
        return -1;
    }
    printk("DTC1 LAB dictionary ready; RAM=%u KiB, Rx<=%u Tx<=%u\n", ram_kib,
           g_ctx.limits.rx_bytes, g_ctx.limits.tx_bytes);
    return 0;
}

void SscStack::ResetRuntimeState() {
    g_ctx.can_dirty = false;
    g_ctx.last_publish = 0;
    g_ctx.published_state = BusinessState::Unconfigured;
    g_ctx.parameters.fill(0);
    g_ctx.rx_image.fill(0);
    g_ctx.tx_image.fill(0);
    g_ctx.feedback = {};
    g_ctx.actions = {};
    g_ctx.mapping_ready = false;
    g_ctx.new_output = false;
    g_ctx.session_stopped = false;
    g_ctx.reset_requested = false;
    g_ctx.data_entry_count = 0;
    layout_.Reset();
}

}  // namespace ecat

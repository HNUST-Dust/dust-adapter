/* SPDX-License-Identifier: Apache-2.0 */

/* SSC 应用回调：AL 状态切换、PDO 映射/拷贝与周期应用入口。 */

#include "ecat/gateway_internal.hpp"

using namespace dust::ecat;

namespace dust::ecat {
namespace gw {

bool mapping_ready;
bool new_output;
volatile bool rearm_pending;
unsigned diag_events;

#if defined(CONFIG_DUST_ECAT_INPUT_LATE_REFRESH)
void RefreshCanBeforeMapping() {
    // One bounded checkpoint in the SSC owner; never consume a pending command
    // or advance UART windows, and never invoke PDO_InputMapping recursively.
    if (!core || core->state() != BusinessState::Active || new_output ||
        !bEcatInputUpdateRunning || !bEcatOutputUpdateRunning ||
        bEscIntEnabled || bDcSyncActive || nAlStatus != STATE_OP ||
        k_is_in_isr()) {
        return;
    }
    can_dirty |= ServiceCanReceive();
    if (!can_dirty) return;
    CaptureCanFeedback(feedback);
    const auto result = core->Publish({tx_image.data(), prepared.tx.bytes},
                                      Now(), feedback, false);
    bool published = result == Error::None;
    if (!published) {
        // Do not submit an old Active image after a rejected late snapshot.
        core->Protect(result);
        feedback = {};
        published = core->Publish({tx_image.data(), prepared.tx.bytes},
                                   Now(), feedback, true) == Error::None;
        if (!published) tx_image.fill(0);
    }
    if (published) {
        can_dirty = false;
        last_publish = Now();
        published_state = core->state();
    }
}
#endif

}  // namespace gw
}  // namespace dust::ecat

using namespace dust::ecat::gw;

extern "C" void APPL_AckErrorInd(UINT16) {}
extern "C" UINT16 APPL_StartMailboxHandler(void) {
    return ALSTATUSCODE_NOERROR;
}
extern "C" UINT16 APPL_StopMailboxHandler(void) {
    return ALSTATUSCODE_NOERROR;
}
extern "C" UINT16 APPL_GenerateMapping(UINT16* input, UINT16* output) {
    *input = 0;
    *output = 0;
    mapping_ready = false;
    const Error snap_err = dictionary.Snapshot(mapped_rx, mapped_tx);
    const Error commit_err =
        snap_err == Error::None
            ? transaction.Commit(limits, mapped_rx, mapped_tx, prepared)
            : snap_err;
    if (commit_err != Error::None ||
        !CanFeedbackCapacityValid(transaction.candidate())) {
        if (diag_events < kDiagLimit) {
            ++diag_events;
            GW_DIAG(
                "Gateway: mapping rejected: snapshot=%u commit=%u capacity=%u "
                "sig=%08x\n",
                unsigned(snap_err), unsigned(commit_err),
                unsigned(CanFeedbackCapacityValid(transaction.candidate())),
                transaction.candidate().signature);
        }
        return ALSTATUSCODE_INVALIDOUTPUTMAPPING;
    }
    ResetMappingObjects();
    if (!Describe(prepared.rx, false) || !Describe(prepared.tx, true)) {
        return ALSTATUSCODE_INVALIDOUTPUTMAPPING;
    }
    *input = prepared.tx.bytes;
    *output = prepared.rx.bytes;
    mapping_ready = true;
    return ALSTATUSCODE_NOERROR;
}
extern "C" UINT16 APPL_StartInputHandler(UINT16*) {
    const bool freerun = sSyncManOutPar.u16SyncType == SYNCTYPE_FREERUN &&
                         sSyncManInPar.u16SyncType == SYNCTYPE_FREERUN;
    const bool sm2 = false;
    const bool dc0 = false;
    if (!mapping_ready) {
        GW_DIAG("Gateway: SAFEOP rejected: mapping not ready\n");
        return ALSTATUSCODE_INVALIDOUTPUTMAPPING;
    }
    if (!CheckSm()) {
        GW_DIAG("Gateway: SAFEOP rejected: SM layout mismatch\n");
        return ALSTATUSCODE_INVALIDOUTPUTMAPPING;
    }
    if (!freerun && !sm2 && !dc0) {
        GW_DIAG("Gateway: SAFEOP rejected: sync type %u/%u unsupported\n",
                sSyncManOutPar.u16SyncType, sSyncManInPar.u16SyncType);
        return ALSTATUSCODE_INVALIDOUTPUTMAPPING;
    }
    const Error cfg_err =
        core->Configure(transaction, limits, mapped_rx, mapped_tx);
    if (cfg_err != Error::None) {
        GW_DIAG("Gateway: SAFEOP rejected: configure error %u\n",
                unsigned(cfg_err));
        return ALSTATUSCODE_INVALIDOUTPUTMAPPING;
    }
    ConfigureIo(transaction.candidate());
    dictionary.Freeze();
    GW_DIAG("Gateway: SAFEOP accepted (Rx=%u Tx=%u)\n", prepared.rx.bytes,
            prepared.tx.bytes);
    return ALSTATUSCODE_NOERROR;
}
extern "C" UINT16 APPL_StopInputHandler(void) {
    if (core && core->state() != BusinessState::Unconfigured) {
        core->Protect(Error::Transport);
        // 会话已结束：Core 处于 Protected，主站下一次配置 0x2100 会被 Write()
        // 以“present device state”拒绝。主循环会在这里之后重建网关。
        rearm_pending = true;
        GW_DIAG("Gateway: SAFEOP stopped; rearm requested (state=%u)\n",
                unsigned(core->state()));
    }
    mapping_ready = false;
    new_output = false;
    rx_image.fill(0);
    tx_image.fill(0);
    return ALSTATUSCODE_NOERROR;
}
extern "C" UINT16 APPL_StartOutputHandler(void) {
    const BusinessState state = core ? core->state() : BusinessState::Unconfigured;
    const bool ok = mapping_ready && core && state == BusinessState::Ready;
    if (!ok) {
        GW_DIAG("Gateway: OP rejected: mapping=%u state=%u\n",
                unsigned(mapping_ready), unsigned(state));
    }
    return ok ? ALSTATUSCODE_NOERROR : ALSTATUSCODE_INVALIDOUTPUTMAPPING;
}
extern "C" UINT16 APPL_StopOutputHandler(void) {
    if (core && core->state() == BusinessState::Active) {
        core->Protect(Error::Transport);
    }
    new_output = false;
    GW_DIAG("Gateway: OP stopped (state=%u)\n",
            core ? unsigned(core->state()) : 0U);
    return ALSTATUSCODE_NOERROR;
}
extern "C" void APPL_OutputMapping(UINT16* data) {
    if (mapping_ready) {
        std::memcpy(rx_image.data(), data, prepared.rx.bytes);
        new_output = true;
    }
}
extern "C" void APPL_InputMapping(UINT16* data) {
    if (mapping_ready) {
#if defined(CONFIG_DUST_ECAT_INPUT_LATE_REFRESH)
        RefreshCanBeforeMapping();
#endif
        std::memcpy(data, tx_image.data(), prepared.tx.bytes);
    }
}
extern "C" void APPL_Application(void) {
    if (!mapping_ready || !core ||
        core->state() == BusinessState::Unconfigured) {
        return;
    }
    const uint64_t now = Now();
    core->Tick(now, bEcatOutputUpdateRunning);
    if (core->state() == BusinessState::Active) {
        can_dirty |= ServiceCanReceive();
    }
    const bool received = new_output;
    if (received) {
        new_output = false;
        const Error process_error = core->Process(
            {rx_image.data(), prepared.rx.bytes}, now, bEcatOutputUpdateRunning,
            actions);
        if (process_error == Error::None) {
            DispatchIo(*core, actions, now);
        } else if (diag_events < kDiagLimit) {
            ++diag_events;
            GW_DIAG("Gateway: RxPDO rejected: err=%u state=%u\n",
                    unsigned(process_error), unsigned(core->state()));
        }
    }
    ServiceIo(*core, now);
    // 常规发布推进 UART 窗口；额外 CAN 发布保留上一份 UART 字节及窗口状态。
    const bool changed = published_state != core->state();
    const bool regular = received || changed ||
        (!bEcatOutputUpdateRunning && now - last_publish >= transaction.candidate().cycle_us);
    if (regular || can_dirty) {
        if (regular) {
            CaptureIo(*core, feedback, now);
        } else {
            CaptureCanFeedback(feedback);
        }

        // 捕获期间 IRQ 仍可入队；发布时刻必须晚于已取出的外设时间戳。
        const Error publish_error = core->Publish(
            {tx_image.data(), prepared.tx.bytes}, Now(), feedback, regular);
        if (publish_error == Error::None) {
            can_dirty = false;
        } else if (diag_events < kDiagLimit) {
            ++diag_events;
            GW_DIAG("Gateway: publish failed: err=%u state=%u\n",
                    unsigned(publish_error), unsigned(core->state()));
        }
        last_publish = now;
        published_state = core->state();
    }
}

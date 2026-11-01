/* SPDX-License-Identifier: Apache-2.0 */

/* 网关生命周期：Core 对象、启动/复位重建、周期 poll 与对外状态查询。 */

#include "ecat/gateway_internal.hpp"

using namespace dust::ecat;

namespace dust::ecat {
namespace gw {

alignas(Core) uint8_t core_storage[sizeof(Core)];
Core* core;
std::array<uint8_t, kMaxImageBytes> rx_image;
std::array<uint8_t, kMaxImageBytes> tx_image;
Core::Feedback feedback;
Actions actions;
bool can_dirty;
uint64_t last_publish = 0;
BusinessState published_state = BusinessState::Unconfigured;

namespace {
void ResetRuntimeState() {
    can_dirty = false;
    last_publish = 0;
    published_state = BusinessState::Unconfigured;
    transaction.Clear();
    dictionary.Clear();
    parameters.fill(0);
    rx_image.fill(0);
    tx_image.fill(0);
    prepared = Layout{};
    feedback = {};
    actions = {};
    mapping_ready = false;
    new_output = false;
    rearm_pending = false;
    diag_events = 0;
}
}  // namespace

}  // namespace gw
}  // namespace dust::ecat

using namespace dust::ecat::gw;

extern "C" int dust_gateway_init(void) {
    if (core) {
        core->~Core();
    }
    core = nullptr;
    uint64_t boot = 0;
    const int initialized = InitializeIo(boot);
    if (initialized || !boot) {
        printk("Gateway IO/boot identity failed: %d\n", initialized);
        return -1;
    }
    core = new (core_storage) Core(uint32_t(boot), IoProtection());
    ResetRuntimeState();
    // 启动/ESC 复位后 ECAT_Init() 重建了 SSC 对象字典；同一会话内的 rearm 只
    // 重建网关状态，必须保留已注册的自定义对象。
    if (OBJ_GetObjectHandle(0x2100) == nullptr && !RegisterDictionaryObjects()) {
        return -1;
    }
    if (!ApplyLimits()) {
        return -1;
    }
    return 0;
}

// 会话结束后的受控重建：新的 boot_id/会话上下文、清空配置暂存和过程影像。
// 调用方必须已屏蔽 ESC IRQ 并清除 stack_ready，本函数不做同步。
extern "C" int dust_gateway_rearm(void) {
    const int result = dust_gateway_init();
    if (result == 0) {
        GW_DIAG("Gateway rearmed for the next session\n");
    }
    return result;
}

extern "C" bool dust_gateway_rearm_pending(void) {
    return rearm_pending;
}

extern "C" void dust_gateway_poll(void) {
    if (!core) {
        return;
    }
    // 0x0440 bit0 是 PD watchdog 触发位（HPM 以低字节在前的 16 位寄存器暴露，
    // 因此单字节读取的 bit0 就是该状态位）；1 表示进程数据仍在刷新。SSC 自己
    // 也会在 CheckIfEcatError() 中检查并转入 SAFEOP/0x1B。
    uint8_t watchdog = 0;
    dust_esc_esc_read(&watchdog, 0x0440, 1);
    const auto now = Now();
    core->Tick(now, bEcatOutputUpdateRunning && (watchdog & 1));
    ServiceIo(*core, now);
    if (core->state() == BusinessState::Active) {
        can_dirty |= ServiceCanReceive();
        // Main-thread FreeRun only; one bounded extra input refresh per poll.
        // Use SSC's mapping/write path instead of bypassing SyncManager handling.
        if (can_dirty && mapping_ready && bEcatInputUpdateRunning &&
            !bEscIntEnabled) {
            CaptureCanFeedback(feedback);
            if (core->Publish({tx_image.data(), prepared.tx.bytes}, Now(),
                              feedback, false) == Error::None) {
                can_dirty = false;
                last_publish = Now();
                published_state = core->state();
                PDO_InputMapping();
            }
        }
    }
}

extern "C" bool dust_gateway_active(void) {
    return core && core->state() == BusinessState::Active;
}

extern "C" void dust_gateway_sync_fault(void) {
    if (core) core->Protect(Error::Transport);
}

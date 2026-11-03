/* SPDX-License-Identifier: Apache-2.0 */

#include "lifecycle.hpp"

#include <cerrno>

namespace ecat {

LifecycleService::LifecycleService(SscStack& stack,
                                   BootAllocator allocate_boot,
                                   Session& session,
                                   ProtectionService& protection,
                                   IoCoordinator& io)
    : stack_(stack),
      allocate_boot_(allocate_boot),
      session_(session),
      protection_(protection),
      io_(io) {}

int LifecycleService::Boot() {
    uint64_t boot = 0;
    int result = allocate_boot_(boot);
    if (result) {
        return result;
    }
    result = io_.Initialize();
    if (result) {
        return result;
    }
    if (io_.ProtectionErrors()) {
        return -EIO;
    }
    protection_.SetInstalled(true);
    session_.Reset(uint32_t(boot), protection_.Hooks());
    result = stack_.Start();
    if (result) {
        protection_.SetInstalled(false);
        return result;
    }
    return 0;
}

void LifecycleService::Enable() {
    stack_.Enable();
    enabled_ = true;
}

bool LifecycleService::Step() {
    if (fatal_) {
        return false;
    }
    const StackEvents events = stack_.TakeEvents();
    if (events.reset_requested || events.session_stopped) {
        Rebuild();
    }
    return !fatal_;
}

void LifecycleService::Rebuild() {
    /* 调用方（SSC 适配器）保证当前处于安全上下文：IRQ 已屏蔽。 */
    stack_.Stop();
    io_.Stop();
    protection_.Reset();
    uint64_t boot = 0;
    if (allocate_boot_(boot) != 0) {
        fatal_ = true;
        return;
    }
    session_.Reset(uint32_t(boot), protection_.Hooks());
    if (stack_.Start() != 0) {
        fatal_ = true;
        return;
    }
    stack_.Enable();
}

}  // namespace ecat

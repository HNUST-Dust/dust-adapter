/* SPDX-License-Identifier: Apache-2.0 */

/* 唯一生命周期状态机：Boot -> Configurable -> Ready/Active -> Protected -> Rearm。
 * AL 状态是输入，IRQ 门控是输出；reset/rearm 只在安全上下文重建。 */
#pragma once

#include <cstdint>

#include "adapters/ethercat_ssc/io_coordinator.hpp"
#include "domain/protection/protection.hpp"
#include "domain/protocol/session.hpp"
#include "adapters/ethercat_ssc/ssc_stack.hpp"

namespace ecat {

class LifecycleService {
   public:
    using BootAllocator = int (*)(uint64_t&);

    LifecycleService(SscStack& stack, BootAllocator allocate_boot,
                     Session& session, ProtectionService& protection,
                     IoCoordinator& io);

    /* 身份 -> IO 初始化 -> 安装保护 -> 启动 SSC 栈。 */
    int Boot();
    /* 置位 stack_ready 并启动 ESC；在板级输出初始化之后调用。 */
    void Enable();

    /* 处理 reset/rearm 事件；返回 false 表示进入不可恢复 Fatal。 */
    bool Step();

    bool Active() const {
        return stack_.Active();
    }
    bool Fatal() const {
        return fatal_;
    }
    bool Enabled() const {
        return enabled_;
    }

   private:
    void Rebuild();

    SscStack& stack_;
    BootAllocator allocate_boot_;
    Session& session_;
    ProtectionService& protection_;
    IoCoordinator& io_;
    bool fatal_ = false;
    bool enabled_ = false;
};

}  // namespace ecat

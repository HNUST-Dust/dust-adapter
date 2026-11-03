/* SPDX-License-Identifier: Apache-2.0 */

/* 单一保护/停机所有者：所有故障先汇聚到这里，再按固定顺序、幂等地执行
 * 本地 safe-stop。服务本身不再直接锁存保护。 */
#pragma once

#include <cstdint>

#include "domain/can/can_service.hpp"
#include "domain/protocol/session.hpp"
#include "domain/uart/uart_service.hpp"

namespace ecat {

class ProtectionService {
   public:
    ProtectionService(Session& session, CanService& can, UartService& uart);

    /* 本地保护是否已完整安装（白名单 + 停机回调）；决定 capability bit31。 */
    void SetInstalled(bool installed);
    Protection Hooks();
    /* reset/rearm：清除锁存，保留累计计数。 */
    void Reset();

    void Report(Error reason);
    FaultSink Sink();

    bool Latched() const {
        return latched_;
    }
    Error LastError() const {
        return last_;
    }
    uint32_t ErrorCount() const {
        return errors_;
    }
    uint32_t ProtectionSignature() const {
        return can_.ProtectionSignature();
    }
    uint32_t CommandTimeoutUs() const {
        return can_.CommandTimeoutUs();
    }

   private:
    static void ReportThunk(void* context, Error reason);
    static bool ValidateCanThunk(void* context, unsigned source,
                                 const CanFrame& frame);
    static void StopThunk(void* context, Error reason);
    void SafeStop(Error reason);

    Session& session_;
    CanService& can_;
    UartService& uart_;
    bool installed_ = false;
    bool latched_ = false;
    Error last_ = Error::None;
    uint32_t errors_ = 0;
};

}  // namespace ecat

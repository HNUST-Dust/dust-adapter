/* SPDX-License-Identifier: Apache-2.0 */

#include "domain/protection/protection.hpp"

namespace ecat {

ProtectionService::ProtectionService(Session& session, CanService& can,
                                     UartService& uart)
    : session_(session), can_(can), uart_(uart) {}

void ProtectionService::SetInstalled(bool installed) {
    installed_ = installed;
}

Protection ProtectionService::Hooks() {
    Protection hooks;
    hooks.context = this;
    hooks.validate_can = ValidateCanThunk;
    hooks.stop = StopThunk;
    hooks.installed = installed_;
    return hooks;
}

void ProtectionService::Reset() {
    latched_ = false;
    last_ = Error::None;
}

void ProtectionService::ReportThunk(void* context, Error reason) {
    static_cast<ProtectionService*>(context)->Report(reason);
}

void ProtectionService::Report(Error reason) {
    if (reason == Error::None) {
        return;
    }
    ++errors_;
    if (latched_) {
        return;
    }
    latched_ = true;
    last_ = reason;
    /* Session::Protect 会在同一上下文同步调用 StopThunk。 */
    session_.Protect(reason);
}

FaultSink ProtectionService::Sink() {
    return {this, &ProtectionService::ReportThunk};
}

bool ProtectionService::ValidateCanThunk(void* context, unsigned,
                                         const CanFrame& frame) {
    auto* self = static_cast<ProtectionService*>(context);
    return self->can_.ValidateFrame(frame);
}

void ProtectionService::StopThunk(void* context, Error reason) {
    static_cast<ProtectionService*>(context)->SafeStop(reason);
}

void ProtectionService::SafeStop(Error) {
    /* 固定顺序、幂等；先停软件来源，再撤硬件在途，最后下发保护帧。
     * 加热器故障属于本地维护，不经过本路径。 */
    uart_.AbortAll();
    can_.ClearAndAbort();
    can_.EmitProtectionFrames();
}

}  // namespace ecat

/* SPDX-License-Identifier: Apache-2.0 */

#include "adapters/ethercat_ssc/io_coordinator.hpp"

#include "common/abi.hpp"

namespace ecat {

IoCoordinator::IoCoordinator(CanService& can, UartService& uart,
                             FeedbackAggregator& feedback)
    : can_(can), uart_(uart), feedback_(feedback) {}

void IoCoordinator::AttachFaultSink(FaultSink sink) {
    fault_sink_ = sink;
}

int IoCoordinator::Initialize() {
    int result = can_.Initialize();
    if (result) {
        return result;
    }
    if (can_.ProtectOnBoot()) {
        can_.EmitProtectionFrames();
    }
    result = uart_.Initialize();
    if (result) {
        return result;
    }
    /* IMU 初始化失败不阻止网关工作；加热器会因无样本而保持关闭。 */
    (void)feedback_.Initialize();
    initialized_ = true;
    return 0;
}

uint32_t IoCoordinator::ProtectionErrors() const {
    return can_.ProtectionErrors() + uart_.ProtectionErrors();
}

bool IoCoordinator::CanCapacityValid(const Configuration& value) const {
    return feedback_.CanCapacityValid(value);
}

void IoCoordinator::ApplyLimits(Limits& limits) const {
    can_.ApplyLimits(limits);
    uart_.ApplyLimits(limits);
    feedback_.ApplyLimits(limits);
}

void IoCoordinator::Configure(const Configuration& value) {
    can_.Configure(value);
    uart_.Configure(value);
    feedback_.Configure(value);
}

void IoCoordinator::EnterActive(const Session& session, uint64_t now) {
    can_.EnterActive(session.active_sources(), now);
    uart_.EnterActive();
}

void IoCoordinator::Stop() {
    can_.Stop();
    uart_.Stop();
}

void IoCoordinator::Service(Session& session, uint64_t now) {

    {

        can_.Pump(now);
    }
    if (session.state() == BusinessState::Active &&
        can_.LeaseExpired(now)) {
        if (fault_sink_.report) {
            fault_sink_.Report(Error::Lease);
        }
        return;
    }
    {

        uart_.Service(session, now);
    }
}

void IoCoordinator::Capture(Session& session, Feedback& feedback,
                            uint64_t now) {
    feedback_.Capture(session, feedback, now);
}

void IoCoordinator::StartGatewayUart(){ uart_.EnterActive(); }
bool IoCoordinator::TakeGatewayUart(unsigned port,UartRxBlock& block){
    return uart_.TakeGatewayRx(port,block);
}



void IoCoordinator::CaptureCan(Feedback& feedback) {
    feedback_.CaptureCan(feedback);
}

bool IoCoordinator::Dispatch(Session& session, const Actions& actions,
                             uint64_t now) {

    if (session.state() != BusinessState::Active) {
        return true;
    }
    if (can_.LeaseExpired(now)) {
        if (fault_sink_.report) {
            fault_sink_.Report(Error::Lease);
        }
        return false;
    }
    if (!can_.Dispatch(actions, now)) {
        return false;
    }
    return uart_.Dispatch(session, actions, now);
}

bool IoCoordinator::DrainCan(uint64_t now) {

    return feedback_.DrainCan(now);
}

int IoCoordinator::QueryCanStatus(unsigned channel, unsigned page,
                                  std::array<uint32_t, 3>& values) const {
    return can_.Status(channel, page, values);
}

int IoCoordinator::QueryUartStatus(unsigned index, unsigned page,
                                   std::array<uint32_t, 3>& values) const {
    return uart_.Status(index, page, values);
}

}  // namespace ecat

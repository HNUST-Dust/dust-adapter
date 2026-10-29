/* SPDX-License-Identifier: Apache-2.0 */
#include "io_backend.hpp"
bool native_io_protected = false;
unsigned native_io_dispatches = 0;
unsigned native_io_stops = 0;
int native_esc_dma_fault = 0;
extern "C" int dust_esc_dma_init(void) {
    return 0;
}
extern "C" int dust_esc_dma_error(void) {
    return native_esc_dma_fault;
}
namespace dust::ecat {
bool native_can_frame_enabled = false, native_can_invalid = false;
bool native_offer_uart = false;
unsigned native_can_checks = 0;
Core::CanEvent native_can_frame{};
namespace {
bool Validate(void*, unsigned, const CanFrame&) {
    return true;
}
void Stop(void*, Error) {
    ++native_io_stops;
}
}  // namespace

int InitializeIo(uint64_t& boot) {
    static uint64_t value = 100;
    boot = ++value;
    return 0;
}
Protection IoProtection() {
    return {nullptr, Validate, Stop, native_io_protected};
}
void ApplyIoLimits(Limits&) {}
void ConfigureIo(const Configuration&) {}
void DispatchIo(Core&, const Actions&, uint64_t) {
    ++native_io_dispatches;
}
bool native_can_dirty = false;
bool CanReceivePending() { return native_can_dirty; }
bool ServiceCanReceive() {
    ++native_can_checks;
    bool value = native_can_dirty;
    native_can_dirty = false;
    return value;
}
void CaptureCanFeedback(Core::Feedback& feedback) {
    if (native_can_frame_enabled) {
        feedback.can[3] = {native_can_frame.sequence, 0, &native_can_frame,
                          uint16_t(native_can_invalid ? 9 : 1)};
    }
}
void ServiceIo(Core&, uint64_t) {}
void CaptureIo(Core& core, Core::Feedback&, uint64_t now) {
    if (native_offer_uart) {
        const uint8_t data[4] = {0x10, 0x20, 0x30, 0x40};
        (void)core.QueueUart(0, {data, sizeof(data)}, uint32_t(now), now);
        native_offer_uart = false;
    }
}
}  // namespace dust::ecat

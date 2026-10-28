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
bool ServiceCanReceive() {
    bool value = native_can_dirty;
    native_can_dirty = false;
    return value;
}
void CaptureCanFeedback(Core::Feedback&) {}
void ServiceIo(Core&, uint64_t) {}
void CaptureIo(Core&, Core::Feedback&, uint64_t) {}
}  // namespace dust::ecat

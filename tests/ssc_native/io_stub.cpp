/* SPDX-License-Identifier: Apache-2.0 */
#include "io_backend.hpp"
bool native_io_protected = false;
unsigned native_io_dispatches = 0;
namespace dust::ecat {
namespace {
bool Validate(void*, unsigned, const CanFrame&) {
    return true;
}
void Stop(void*, Error) {}
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
void ServiceIo(Core&, uint64_t) {}
void CaptureIo(Core&, Core::Feedback&, uint64_t) {}
}  // namespace dust::ecat

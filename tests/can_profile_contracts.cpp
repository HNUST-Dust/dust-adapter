/* SPDX-License-Identifier: Apache-2.0 */
#include <cstdlib>
#include <iostream>

#include "client.hpp"
#include "drivers/firmware_can_profile.hpp"
#define CHECK(c)                                      \
    do {                                              \
        if (!(c)) {                                   \
            std::cerr << "line " << __LINE__ << '\n'; \
            std::abort();                             \
        }                                             \
    } while (0)
int main() {
    namespace profile = dust::can_profile;
    namespace wire = dust::wire;
    CHECK(profile::kProtectionFrames.size() == 6);
    CHECK(profile::kReceiveFilters.size() == 6);
    CHECK(profile::kProtectOnBoot);
    CHECK(wire::ProtectionSignature(profile::kCommandTimeoutUs,
                                    profile::kProtectionFrames.data(),
                                    profile::kProtectionFrames.size()) ==
          profile::kProtectionSignature);
    auto request = dust::SafetyConfiguration(1, 99, 20000, nullptr, 0);
    CHECK(profile::AcceptsSafetyRequest(
        request));  // default selection, never an empty protection table
    CHECK(profile::kProtectionFrames.size() == 6);
    request = dust::SafetyConfiguration(2, 99, 20000,
                                        profile::kProtectionFrames.data(),
                                        profile::kProtectionFrames.size());
    CHECK(profile::AcceptsSafetyRequest(request));
    request[32 + 8] ^= 1;
    CHECK(!profile::AcceptsSafetyRequest(request));
    request = dust::SafetyConfiguration(3, 99, 21000, nullptr, 0);
    CHECK(!profile::AcceptsSafetyRequest(
        request));  // host cannot extend the firmware limit
    request = dust::SafetyConfiguration(4, 99, 10000, nullptr, 0);
    CHECK(profile::AcceptsSafetyRequest(
        request));  // stricter host lease is allowed
    auto frame = profile::kProtectionFrames.front();
    CHECK(profile::AllowsTransmit(frame));
    frame.channel = 0;
    CHECK(!profile::AllowsTransmit(frame));
    frame.channel = 3;
    frame.id = 0x555;
    CHECK(!profile::AllowsTransmit(frame));
    frame.id = 0x200;
    frame.flags = wire::kExtended;
    CHECK(!profile::AllowsTransmit(frame));
    frame.flags = 0;
    CHECK(profile::AllowsTransmit(frame));
    wire::Record uart{};
    uart.channel = 2;
    uart.flags = wire::kUartAction;
    uart.length = 1;
    uart.data[0] = 0;
    request = dust::SafetyConfiguration(5, 99, 20000, &uart, 1);
    CHECK(profile::AcceptsSafetyRequest(
        request));  // UART may supplement, not replace, the CAN baseline
    request = dust::SafetyConfiguration(6, 99, 20000,
                                        profile::kProtectionFrames.data(), 1);
    CHECK(!profile::AcceptsSafetyRequest(
        request));  // partial CAN baseline cannot be confirmed
    auto changed = profile::kProtectionFrames;
    changed[0].data[0] ^= 1;
    CHECK(wire::ProtectionSignature(20000, changed.data(), changed.size()) !=
          profile::kProtectionSignature);
    CHECK(wire::ProtectionSignature(19999, profile::kProtectionFrames.data(),
                                    6) != profile::kProtectionSignature);
    std::cout << "Firmware CAN allowlist, default selection, signature and "
                 "anti-override contracts passed\n";
}

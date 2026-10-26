/* SPDX-License-Identifier: Apache-2.0 */
#include <cstdlib>
#include <iostream>

#include "drivers/firmware_can_profile.hpp"
#include "usb_protocol.hpp"
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
    dust::wire::UsbOutRequest request{};
    wire::ConfigureSafetyRequest safety{};
    safety.header = {1, 99};
    safety.lease_us = 20000;
    CHECK(wire::Encode(safety, request));
    CHECK(profile::AcceptsSafetyRequest(
        request));  // default selection, never an empty protection table
    CHECK(profile::kProtectionFrames.size() == 6);
    // 故意构造非法的 CAN 保护上传包，验证固件拒绝旧确认行为。
    request[10] = profile::kProtectionFrames.size();
    for (size_t i = 0; i < profile::kProtectionFrames.size(); ++i) {
        wire::Encode(request.data() + 32 + i * wire::kRecordBytes,
                     profile::kProtectionFrames[i]);
    }
    CHECK(!profile::AcceptsSafetyRequest(request));
    request[32 + 8] ^= 1;
    CHECK(!profile::AcceptsSafetyRequest(request));
    safety.lease_us = 21000;
    CHECK(wire::Encode(safety, request));
    CHECK(!profile::AcceptsSafetyRequest(
        request));  // host cannot extend the firmware limit
    safety.lease_us = 10000;
    CHECK(wire::Encode(safety, request));
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
    safety.lease_us = 20000;
    safety.uart_stop_count = 1;
    safety.uart_stop_actions[0].channel = 2;
    safety.uart_stop_actions[0].length = 1;
    CHECK(wire::Encode(safety, request));
    CHECK(profile::AcceptsSafetyRequest(
        request));  // UART may supplement, not replace, the CAN baseline
    request[10] = 1;
    wire::Encode(request.data() + 32, profile::kProtectionFrames.front());
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

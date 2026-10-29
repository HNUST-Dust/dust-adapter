/* SPDX-License-Identifier: Apache-2.0 */
#include <cassert>
#include "drivers/firmware_can_profile.hpp"
int main() {
    namespace p = dust::can_profile;
    assert(p::kProtectionFrames.size() == 6);
    assert(p::kReceiveFilters.size() == 6);
    assert(p::kProtectOnBoot);
    assert(p::kProtectionSignature == 0x9fb5fef5U);
    auto frame = p::kProtectionFrames.front();
    assert(frame.channel == 3 && frame.id == 0x200 && frame.length == 8);
    for (auto byte : frame.data) assert(byte == 0);
    assert(p::AllowsTransmit(frame));
    frame.channel = 0; assert(!p::AllowsTransmit(frame));
    frame.channel = 3; frame.id = 0x555; assert(!p::AllowsTransmit(frame));
    frame.id = 0x200; frame.flags = 1; assert(!p::AllowsTransmit(frame));
    frame.flags = 0; assert(p::AllowsTransmit(frame));
}

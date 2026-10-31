/* SPDX-License-Identifier: Apache-2.0 */
#include <cassert>
#include <cstdint>
#include "ethercat/latest_can.hpp"
int main() {
    using namespace dust::ecat;
    LatestCanSlots slots;
    uint32_t ids[] = {0x201, 0x10, 0x13};
    assert(slots.Configure(ids, 3));
    Core::CanEvent state{}; state.id = 0x201; state.capture_us = 100; state.sequence = 1; state.length = 8;
    Core::CanEvent reply{}; reply.id = 0x13; reply.capture_us = 110; reply.sequence = 2; reply.length = 8;
    assert(slots.Update(state) && slots.Update(reply));
    state.sequence = 3; state.capture_us = 120; assert(slots.Update(state));
    assert(slots.Valid(0) && slots.Frame(0).sequence == 3);
    assert(slots.Valid(2) && slots.Frame(2).sequence == 2);
    assert(!slots.Valid(1));
    Core::CanEvent unknown{}; unknown.id = 0x55; assert(!slots.Update(unknown));
    uint32_t invalid[] = {0x201, 0x201}; assert(!slots.Configure(invalid, 2));
    assert(slots.Count() == 3 && slots.Frame(2).sequence == 2);
    assert(slots.Expire(210, 100) && !slots.Valid(2) && slots.Valid(0));
    state.capture_us = 0xfffffff0U; assert(slots.Update(state));
    assert(!slots.Expire(0x10U, 100));
    assert(slots.Expire(0x54U, 100) && !slots.Valid(0));
    return 0;
}

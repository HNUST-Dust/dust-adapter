/* SPDX-License-Identifier: Apache-2.0 */
#include <cassert>
#include <cstdint>
uint64_t trace_test_clock = 0;
#include "../apps/ethercat_slave/src/rtt_trace.cpp"
int main() {
    dust_trace_reset(7);
    dust_trace_stamp(DUST_TR_CAN_RX,
                     9);  // startup feedback must not create records
    assert(dust_ecat_trace.count == 0);
    dust_trace_read(DUST_TR_ESC_READ_BEGIN, 0x1100, 96);
    dust_trace_read(DUST_TR_ESC_READ_END, 0x1100, 96);
    dust_trace_mapping(11, 123);
    dust_trace_process();
    dust_trace_processed();
    dust_trace_begin(11, 3);
    assert(dust_ecat_trace.count == 1);
    auto& r = dust_ecat_trace.records[0];
    dust_trace_stamp(DUST_TR_PUBLISH, 99);
    dust_trace_input(99, 400);
    dust_trace_esc(DUST_TR_ESC_BEGIN, 0x1d00, 400);
    assert(!(r.valid & (1U << DUST_TR_PUBLISH)));
    dust_trace_stamp(DUST_TR_SUBMIT, 0);
    dust_trace_stamp(DUST_TR_CAN_RX, 9);
    dust_trace_capture(8);
    assert(!(r.valid & (1U << DUST_TR_CAPTURE)));
    dust_trace_can_queue(DUST_TR_CAN_ENQUEUED, 8);
    assert(!(r.valid & (1U << DUST_TR_CAN_ENQUEUED)));
    dust_trace_can_queue(DUST_TR_CAN_ENQUEUED, 9);
    dust_trace_can_queue(DUST_TR_CAN_DEQUEUED, 9);
    assert(r.cycles[DUST_TR_CAN_DEQUEUED] > r.cycles[DUST_TR_CAN_ENQUEUED]);
    dust_trace_capture(9);
    dust_trace_stamp(DUST_TR_PUBLISH, 99);
    dust_trace_input(98, 400);
    assert(!(r.valid & (1U << DUST_TR_INPUT_MAP)));
    dust_trace_input(100, 400);
    dust_trace_esc(DUST_TR_ESC_BEGIN, 0x1000, 400);
    dust_trace_esc(DUST_TR_ESC_BEGIN, 0x1d00, 399);
    assert(!(r.valid & (1U << DUST_TR_ESC_BEGIN)));
    dust_trace_esc(DUST_TR_ESC_BEGIN, 0x1d00, 400);
    dust_trace_esc(DUST_TR_ESC_END, 0x1d00, 400);
    auto first = r.cycles[DUST_TR_ESC_END];
    dust_trace_esc(DUST_TR_ESC_END, 0x1d00, 400);
    assert(r.cycles[DUST_TR_ESC_END] == first && r.publication == 99 &&
           r.can_sequence == 9);
    assert(!(r.revision & 1));
    for (unsigned i = 1; i < 128; ++i) {
        dust_trace_begin(11, i + 3);
    }
    dust_trace_begin(11, 200);
    dust_trace_stamp(DUST_TR_CAN_RX, 77);
    assert(dust_ecat_trace.count == 128 && dust_ecat_trace.overflow == 1);
    assert(dust_ecat_trace.records[127].can_sequence == 0);
    dust_trace_reset(8);
    assert(dust_ecat_trace.count == 0 && dust_ecat_trace.boot == 8);
    dust_trace_mapping(12, 1);
    dust_trace_process();
    dust_trace_begin(13, 1);
    assert(dust_ecat_trace.count == 0);
}

/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
struct hpm_mcan_rx_timing;
enum dust_trace_stage {
    DUST_TR_ESC_READ_BEGIN,
    DUST_TR_ESC_READ_END,
    DUST_TR_RX_MAP,
    DUST_TR_PROCESS,
    DUST_TR_PROCESS_DONE,
    DUST_TR_ACCEPT,
    DUST_TR_ENQUEUE,
    DUST_TR_SUBMIT,
    DUST_TR_SUBMITTED,
    DUST_TR_TX_DONE,
    DUST_TR_CAN_RX,
    DUST_TR_CAPTURE,
    DUST_TR_PUBLISH,
    DUST_TR_INPUT_MAP,
    DUST_TR_ESC_BEGIN,
    DUST_TR_ESC_END,
    DUST_TR_CAN_ENQUEUED,
    DUST_TR_CAN_DEQUEUED,
    DUST_TR_CAN_SNAPSHOT,
    DUST_TR_WORKER_WAKE,
    DUST_TR_WORKER_LOCKED,
    DUST_TR_CAN3_SCAN,
    DUST_TR_WORKER_WAIT,
    DUST_TR_SSC_SERVICE,
    DUST_TR_SNAPSHOT_BEGIN,
    DUST_TR_SNAPSHOT_LOCKED,
    DUST_TR_PENDING_WRITE_END,
    DUST_TR_SSC_RETURN,
    DUST_TR_POLL_ENTRY,
    DUST_TR_STAGES
};
// Written only by the SSC owner; captured by the worker under trace IRQ lock.
enum dust_trace_phase_id {
    DUST_PHASE_UNKNOWN, DUST_PHASE_OUTER, DUST_PHASE_SSC,
    DUST_PHASE_MAINTENANCE, DUST_PHASE_WAIT,
    DUST_PHASE_POLL_TICK, DUST_PHASE_POLL_IO, DUST_PHASE_POLL_CAN,
    DUST_PHASE_POLL_PUBLISH, DUST_PHASE_APP_TICK, DUST_PHASE_APP_CAN,
    DUST_PHASE_APP_PROCESS, DUST_PHASE_APP_IO, DUST_PHASE_APP_PUBLISH,
    DUST_PHASE_ESC_READ, DUST_PHASE_ESC_WRITE
};
#if defined(CONFIG_DUST_ECAT_RTT_TRACE)
unsigned dust_trace_phase(unsigned phase);
void dust_trace_reset(uint64_t boot);
#if defined(CONFIG_HPM_MCAN_RX_TIMING)
void dust_trace_can_timing(uint32_t sequence, const struct hpm_mcan_rx_timing *timing);
#endif
void dust_trace_mapping(uint64_t session, uint32_t exchange);
void dust_trace_process(void);
void dust_trace_processed(void);
void dust_trace_read(unsigned stage, uint16_t address, uint16_t bytes);
void dust_trace_begin(uint64_t session, uint32_t batch);
void dust_trace_stamp(unsigned stage, uint32_t value);
void dust_trace_capture(uint32_t can_sequence);
void dust_trace_can_queue(unsigned stage, uint32_t can_sequence);
void dust_trace_input(uint32_t publication, uint32_t bytes);
void dust_trace_esc(unsigned stage, uint16_t address, uint16_t bytes);
#else
static inline unsigned dust_trace_phase(unsigned phase) {
    (void)phase;
    return DUST_PHASE_UNKNOWN;
}
static inline void dust_trace_reset(uint64_t b) {
    (void)b;
}
static inline void dust_trace_mapping(uint64_t s, uint32_t e) {
    (void)s;
    (void)e;
}
static inline void dust_trace_process(void) {}
static inline void dust_trace_processed(void) {}
static inline void dust_trace_read(unsigned s, uint16_t a, uint16_t b) {
    (void)s;
    (void)a;
    (void)b;
}
static inline void dust_trace_begin(uint64_t s, uint32_t b) {
    (void)s;
    (void)b;
}
static inline void dust_trace_stamp(unsigned s, uint32_t v) {
    (void)s;
    (void)v;
}
static inline void dust_trace_can_queue(unsigned stage, uint32_t s) {
    (void)stage;
    (void)s;
}
static inline void dust_trace_capture(uint32_t s) {
    (void)s;
}
static inline void dust_trace_input(uint32_t p, uint32_t b) {
    (void)p;
    (void)b;
}
static inline void dust_trace_esc(unsigned s, uint16_t a, uint16_t b) {
    (void)s;
    (void)a;
    (void)b;
}
#endif
#ifdef __cplusplus
}
// Restore the caller's phase even on early return from an application callback.
class DustTracePhaseScope {
    unsigned previous_;
public:
    explicit DustTracePhaseScope(unsigned phase) : previous_(dust_trace_phase(phase)) {}
    ~DustTracePhaseScope() { (void)dust_trace_phase(previous_); }
    DustTracePhaseScope(const DustTracePhaseScope&) = delete;
    DustTracePhaseScope& operator=(const DustTracePhaseScope&) = delete;
};
#endif

/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
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
    DUST_TR_STAGES
};
#if defined(CONFIG_DUST_ECAT_RTT_TRACE)
void dust_trace_reset(uint64_t boot);
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
#endif

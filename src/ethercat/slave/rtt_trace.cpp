/* SPDX-License-Identifier: Apache-2.0 */
#include "rtt_trace.h"
#if defined(CONFIG_DUST_ECAT_RTT_TRACE)
#include <zephyr/irq.h>
#include <zephyr/kernel.h>

#include <cstddef>
#include <cstring>
#if defined(CONFIG_HPM_MCAN_RX_TIMING)
#include <drivers/can/hpmicro_mcan_timing.h>
#endif
// Single outstanding DM disable command only. No logging/allocation in IRQs.
struct TraceRecord {
    uint32_t revision, valid;
    uint64_t session;
    uint32_t batch, exchange, publication, can_sequence;
    int32_t submit_result, done_result;
    uint64_t cycles[DUST_TR_STAGES];
};
struct TraceBuffer {
    uint32_t magic, version, record_bytes, capacity, count, overflow, clock_hz,
        header_bytes;
    uint64_t boot;
    TraceRecord records[128];
};
static_assert(sizeof(TraceRecord) == 184 &&
              offsetof(TraceBuffer, records) == 40);
extern "C" {
TraceBuffer dust_ecat_trace;
#if defined(CONFIG_HPM_MCAN_RX_TIMING)
struct CanTimingRecord {
    uint32_t revision, valid;
    uint64_t session;
    uint32_t batch, sequence;
    hpm_mcan_rx_timing timing;
};
struct CanTimingBuffer {
    uint32_t magic, version, record_bytes, capacity, count, overflow, clock_hz, header_bytes;
    uint64_t boot;
    CanTimingRecord records[128];
};
static_assert(sizeof(hpm_mcan_rx_timing) == 48 && sizeof(CanTimingRecord) == 72 &&
              offsetof(CanTimingBuffer, records) == 40);
CanTimingBuffer dust_mcan_timing;
#endif
}
namespace {
TraceRecord* active;
uint64_t mapped_session, mapped_at, process_at, processed_at, read_begin,
    read_end;
uint32_t mapped_exchange, input_bytes;
bool Has(unsigned stage) {
    return active && (active->valid & (1U << stage));
}
void Stamp(unsigned stage, uint64_t at, uint32_t value) {
    if (!active || stage >= DUST_TR_STAGES || Has(stage)) {
        return;
    }
    ++active->revision;
    active->cycles[stage] = at;
    if (stage == DUST_TR_CAN_RX) {
        active->can_sequence = value;
    }
    if (stage == DUST_TR_PUBLISH) {
        active->publication = value;
    }
    if (stage == DUST_TR_SUBMITTED) {
        active->submit_result = int32_t(value);
    }
    if (stage == DUST_TR_TX_DONE) {
        active->done_result = int32_t(value);
    }
    active->valid |= 1U << stage;
    ++active->revision;
}
}  // namespace
extern "C" void dust_trace_reset(uint64_t boot) {
    const auto key = irq_lock();
    std::memset(&dust_ecat_trace, 0, sizeof(dust_ecat_trace));
#if defined(CONFIG_HPM_MCAN_RX_TIMING)
    std::memset(&dust_mcan_timing, 0, sizeof(dust_mcan_timing));
    dust_mcan_timing.magic = 0x3154434d; // MCT1, independent optional sidecar.
    dust_mcan_timing.version = 1;
    dust_mcan_timing.record_bytes = sizeof(CanTimingRecord);
    dust_mcan_timing.capacity = 128;
    dust_mcan_timing.clock_hz = CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC;
    dust_mcan_timing.header_bytes = 40;
    dust_mcan_timing.boot = boot;
#endif
    dust_ecat_trace.magic = 0x31525444;  // DTR1
    dust_ecat_trace.version = 2;
    dust_ecat_trace.record_bytes = sizeof(TraceRecord);
    dust_ecat_trace.capacity = 128;
    dust_ecat_trace.clock_hz = CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC;
    dust_ecat_trace.header_bytes = 40;
    dust_ecat_trace.boot = boot;
    active = nullptr;
    mapped_at = process_at = processed_at = mapped_session = read_begin =
        read_end = 0;
    input_bytes = 0;
    irq_unlock(key);
}
extern "C" void dust_trace_mapping(uint64_t session, uint32_t exchange) {
    const auto key = irq_lock();
    mapped_at = k_cycle_get_64();
    mapped_session = session;
    mapped_exchange = exchange;
    irq_unlock(key);
}
extern "C" void dust_trace_process(void) {
    const auto key = irq_lock();
    process_at = k_cycle_get_64();
    irq_unlock(key);
}
extern "C" void dust_trace_processed(void) {
    const auto key = irq_lock();
    processed_at = k_cycle_get_64();
    irq_unlock(key);
}
extern "C" void dust_trace_read(unsigned stage, uint16_t address,
                                uint16_t bytes) {
    if (address != 0x1100 || bytes < 64) {
        return;
    }
    const auto key = irq_lock();
    if (stage == DUST_TR_ESC_READ_BEGIN) {
        read_begin = k_cycle_get_64();
    }
    if (stage == DUST_TR_ESC_READ_END) {
        read_end = k_cycle_get_64();
    }
    irq_unlock(key);
}
extern "C" void dust_trace_begin(uint64_t session, uint32_t batch) {
    const auto key = irq_lock();
    active = nullptr;
    input_bytes = 0;
    if (dust_ecat_trace.count == dust_ecat_trace.capacity) {
        ++dust_ecat_trace.overflow;
    } else if (session == mapped_session) {
        active = &dust_ecat_trace.records[dust_ecat_trace.count++];
        active->session = session;
        active->batch = batch;
        active->exchange = mapped_exchange;
        Stamp(DUST_TR_ESC_READ_BEGIN, read_begin, 0);
        Stamp(DUST_TR_ESC_READ_END, read_end, 0);
        Stamp(DUST_TR_PROCESS_DONE, processed_at, 0);
        Stamp(DUST_TR_RX_MAP, mapped_at, 0);
        Stamp(DUST_TR_PROCESS, process_at, 0);
        Stamp(DUST_TR_ACCEPT, k_cycle_get_64(), 0);
    }
    irq_unlock(key);
}
extern "C" void dust_trace_stamp(unsigned stage, uint32_t value) {
    const auto key = irq_lock();
    // A publication is eligible only after the matching CAN event was captured.
    if ((stage != DUST_TR_PUBLISH || Has(DUST_TR_CAPTURE)) &&
        (stage != DUST_TR_CAN_RX || Has(DUST_TR_SUBMIT))) {
        Stamp(stage, k_cycle_get_64(), value);
    }
    irq_unlock(key);
}
#if defined(CONFIG_HPM_MCAN_RX_TIMING)
extern "C" void dust_trace_can_timing(uint32_t sequence, const hpm_mcan_rx_timing *timing) {
    if (!timing) return;
    const auto key = irq_lock();
    if (Has(DUST_TR_CAN_RX) && active->can_sequence == sequence) {
        const auto index = unsigned(active - dust_ecat_trace.records);
        auto& record = dust_mcan_timing.records[index];
        if (!record.valid) {
            ++record.revision;
            record.session = active->session;
            record.batch = active->batch;
            record.sequence = sequence;
            record.timing = *timing;
            record.valid = 1;
            ++record.revision;
            if (dust_mcan_timing.count <= index) dust_mcan_timing.count = index + 1;
        }
    }
    irq_unlock(key);
}
#endif
extern "C" void dust_trace_can_queue(unsigned stage, uint32_t sequence) {
    const auto key = irq_lock();
    if ((stage == DUST_TR_CAN_ENQUEUED || stage == DUST_TR_CAN_DEQUEUED) &&
        Has(DUST_TR_CAN_RX) && active->can_sequence == sequence) {
        Stamp(stage, k_cycle_get_64(), 0);
    }
    irq_unlock(key);
}
extern "C" void dust_trace_capture(uint32_t sequence) {
    const auto key = irq_lock();
    if (Has(DUST_TR_CAN_RX) && active->can_sequence == sequence) {
        Stamp(DUST_TR_CAPTURE, k_cycle_get_64(), 0);
    }
    irq_unlock(key);
}
extern "C" void dust_trace_input(uint32_t publication, uint32_t bytes) {
    const auto key = irq_lock();
    if (Has(DUST_TR_PUBLISH) &&
        uint32_t(publication - active->publication) < 0x80000000U) {
        Stamp(DUST_TR_INPUT_MAP, k_cycle_get_64(), 0);
        input_bytes = bytes;
    }
    irq_unlock(key);
}
extern "C" void dust_trace_esc(unsigned stage, uint16_t address,
                               uint16_t bytes) {
    const auto key = irq_lock();
    if (Has(DUST_TR_INPUT_MAP) && address == 0x1d00 && bytes == input_bytes &&
        (stage == DUST_TR_ESC_BEGIN ||
         (stage == DUST_TR_ESC_END && Has(DUST_TR_ESC_BEGIN)))) {
        Stamp(stage, k_cycle_get_64(), 0);
    }
    irq_unlock(key);
}
#endif

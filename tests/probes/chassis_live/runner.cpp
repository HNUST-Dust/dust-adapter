// SPDX-License-Identifier: Apache-2.0
#include <sched.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>

#include "common/device_pdo.hpp"
#include "common/device_sensor_page.hpp"
#include "hardware/imu/hi91_protocol.h"
#include "hardware/remote/wfly_sbus_protocol.h"
#include "hardware/ethercat/transport.hpp"

#include "controller_bridge.hpp"

namespace ec = dust_control::ethercat;
namespace wire = dust::device_pdo;

uint64_t Now() {
    timespec t{};
    clock_gettime(CLOCK_MONOTONIC, &t);
    return uint64_t(t.tv_sec) * 1000000000 + t.tv_nsec;
}

struct Timing {
    uint64_t count = 0, sum = 0, max = 0, min = UINT64_MAX;
    void Add(uint64_t ns) {
        ++count;
        sum += ns;
        max = std::max(max, ns);
        min = std::min(min, ns);
    }
    void Print() const {
        printf("{\"count\":%llu,\"mean_us\":%.2f,\"min_us\":%.2f,\"max_us\":%.2f}",
               (unsigned long long)count,
               count ? double(sum) / count / 1000 : 0,
               count ? double(min) / 1000 : 0, double(max) / 1000);
    }
};

struct ArrivalTiming : Timing {
    std::vector<uint64_t> intervals;

    void Add(uint64_t ns) {
        Timing::Add(ns);
        if (intervals.size() < intervals.capacity()) {
            intervals.push_back(ns);
        }
    }

    void Print() {
        std::sort(intervals.begin(), intervals.end());
        printf("{\"count\":%llu,\"mean_us\":%.2f,\"min_us\":%.2f,\"max_us\":%.2f",
               (unsigned long long)count, count ? double(sum) / count / 1000 : 0,
               count ? double(min) / 1000 : 0, double(max) / 1000);
        if (count && intervals.size() == count) {
            const auto percentile = [&](size_t percent) {
                const auto rank = (intervals.size() * percent + 99) / 100;
                return double(intervals[rank - 1]) / 1000;
            };
            printf(",\"p50_us\":%.2f,\"p99_us\":%.2f", percentile(50), percentile(99));
        } else {
            printf(",\"p50_us\":null,\"p99_us\":null");
        }
        printf("}");
    }
};

struct PhaseSample {
    uint64_t previous = 0;
    unsigned missing = 0, reused = 0, over_1ms = 0;
    Timing age;

    void Observe(uint64_t sample_at, uint64_t now) {
        if (!sample_at) {
            ++missing;
            return;
        }
        reused += sample_at == previous;
        previous = sample_at;
        age.Add(now - sample_at);
        over_1ms += now - sample_at > 1000000;
    }
};

int main(int argc, char** argv) {
    const auto seconds = argc > 1 ? std::strtoul(argv[1], nullptr, 10) : 20;
    const bool physical_live = argc > 10 && std::string_view(argv[10]) == "live-control";
    const bool safe_can = physical_live || (argc > 10 && std::string_view(argv[10]) == "safe-can");
    if (!safe_can) {
        fprintf(stderr, "Use explicit safe-can (no controller output) or live-control (signal and remote gated)\n");
        return 2;
    }
    const bool sensors = argc > 2 && std::string_view(argv[2]) == "sensor60";
    const bool shadow = sensors && argc > 3 && std::string_view(argv[3]) == "shadow1k";
    const bool no_hi91_mode = shadow && argc > 4 && std::string_view(argv[4]) == "no-hi91";
    const bool event_only = shadow && argc > 4 &&
                            std::string_view(argv[4]) == "motors-imu-event";
    const bool imu_latency = event_only && argc > 5 &&
                             std::string_view(argv[5]) == "imu-latency";
    const bool live_full = event_only && argc > 5 &&
                           std::string_view(argv[5]) == "live-full";
    const bool combined = live_full && argc > 6 &&
                          std::string_view(argv[6]) == "combined";
    const bool uart_no_ack = combined && argc > 7 &&
                             std::string_view(argv[7]) == "no-ack";
    const bool hi91_compact = combined && argc > 7 &&
                              std::string_view(argv[7]) == "hi91-compact";
    const bool dm_disable = hi91_compact && argc > 8 &&
                            std::string_view(argv[8]) == "dm-disable";
    unsigned dm_stagger_slots = 0;
    if (dm_disable && argc > 9) {
        const std::string_view phase(argv[9]);
        if (phase == "125") {
            dm_stagger_slots = 1;
        } else if (phase == "250") {
            dm_stagger_slots = 2;
        } else if (phase != "0") {
            fprintf(stderr, "Invalid DM phase offset\n");
            return 2;
        }
    }
    const bool early_retry = combined && argc > 7 &&
                             std::string_view(argv[7]) == "window2-early";
    const bool hi91_window = early_retry || (combined && argc > 7 &&
                             std::string_view(argv[7]) == "window2");
    const std::string_view uart_mode =
        live_full && argc > 6 && !combined ? argv[6] : "both";
    if (uart_mode != "both" && uart_mode != "none" &&
        uart_mode != "hi91" && uart_mode != "sbus") {
        fprintf(stderr, "Invalid UART test mode\n");
        return 2;
    }
    const bool disable_hi91 = no_hi91_mode || uart_mode == "none" || uart_mode == "sbus";
    const bool disable_sbus = uart_mode == "none" || uart_mode == "hi91";
    const bool live_shadow = live_full || (event_only && argc > 5 &&
                             std::string_view(argv[5]) == "live-shadow");
    const bool motors_imu = event_only || (shadow && argc > 4 &&
                            std::string_view(argv[4]) == "motors-imu");
    const bool motors_only = (motors_imu && !live_full) ||
        (shadow && argc > 4 && std::string_view(argv[4]) == "motors-only");
    const bool batch =
        sensors || (argc > 2 && std::string_view(argv[2]) == "motor60");
    const size_t image_size = batch ? wire::kMotorBatchBytes : wire::kBytes;
    size_t tx_size = combined ? 180 : image_size;
    if (event_only && argc > 6 && std::string_view(argv[5]) == "tx-sweep") {
        char* end = nullptr;
        tx_size = std::strtoul(argv[6], &end, 10);
        if (*end || tx_size < image_size ||
            tx_size > dust::device_sensor_page::kSweepMaxBytes || (tx_size & 1U)) {
            fprintf(stderr, "Invalid TX sweep size\n");
            return 2;
        }
    }
    auto config = ec::ProductionPdoConfig();
    auto& capacity = config.adapters.at("base");
    capacity.vendor_id = 3415;
    capacity.product_code = 0x44544331;
    capacity.can_tx_slots = {};
    capacity.can_rx_slots = wire::kCanRxCounts;
    capacity.uart = {};
    if (batch && !sensors) {
        capacity.onboard_imu = false;
    }
    auto layout = ec::BuildPdoLayout(config).adapters.at("base");
    layout.layout_signature = 0x44543130;
    layout.rx = {10, {{0x1600, {{"device.rx", 0x2050, 1, 80, 0}}}}};
    layout.tx = {10, {{0x1a00, {{"device.tx", 0x3051, 1, 80, 0}}}}};
    if (batch) {
        layout.layout_signature = sensors ? dust::device_sensor_page::kSignature
                                          : wire::kMotorBatchSignature;
        layout.rx.bytes = layout.tx.bytes = image_size;
        for (unsigned slot = 1; slot < 6; ++slot) {
            layout.rx.mappings[0].entries.push_back(
                {"device.rx", 0x2050, uint8_t(slot + 1), 80, slot * 10});
            layout.tx.mappings[0].entries.push_back(
                {"device.tx", 0x3051, uint8_t(slot + 1), 80, slot * 10});
        }
    }
    for (size_t offset = image_size, subindex = 7; offset < tx_size; ++subindex) {
        const auto width = std::min(dust::device_sensor_page::kSweepEntryBytes,
                                    tx_size - offset);
        layout.tx.mappings[0].entries.push_back(
            {"device.tx", 0x3051, uint8_t(subindex), uint8_t(width * 8), uint32_t(offset)});
        offset += width;
    }
    layout.tx.bytes = tx_size;
    std::vector<ec::BoardImage> boards{
        {"base", capacity, layout, std::vector<uint8_t>(image_size),
         std::vector<uint8_t>(tx_size), false, false}};
    live_test::Bridge bridge(physical_live,argc>11?argv[11]:"/home/wbr-infantry/dust-control/config");
    bridge.Start();
    auto link = ec::CreateIghTransport();
    std::string error;
    if (!link->Open(0, boards, error)) {
        fprintf(stderr, "%s\n", error.c_str());
        return 2;
    }
    cpu_set_t cpu;
    CPU_ZERO(&cpu);
    CPU_SET(3, &cpu);
    sched_param priority{80};
    if (sched_setaffinity(0, sizeof(cpu), &cpu) ||
        sched_setscheduler(0, SCHED_FIFO, &priority) ||
        mlockall(MCL_CURRENT | MCL_FUTURE)) {
        perror("RT setup");
        link->Close();
        return 2;
    }
    wire::Payload token{};
    const uint32_t nonce = uint32_t(Now()) | 1;
    for (unsigned i = 0; i < 4; ++i) {
        token[i] = uint8_t(nonce >> (8 * i));
    }
    wire::ImuAssembler imu(20000);
    wire::ImuSample sample{};
    std::array<uint8_t, 6> previous{};
    std::array<uint64_t, 6> motor_at{};
    std::array<ArrivalTiming, 6> motor_interval{};
    ArrivalTiming imu_interval;
    Timing imu_capture_interval, rtt, wake_lateness, esc_write, page_to_esc;
    uint32_t last_imu_capture = 0, last_imu_sequence = 0;
    uint64_t imu_skipped_source_samples = 0;
    std::array<uint64_t, 6> esc_matches{}, esc_long_gap_matches{};
    std::array<uint32_t, 6> last_traced_capture{};
    std::array<bool, 6> have_trace{}, last_motor_gap_long{};
    struct DmTrace {
        uint32_t capture = 0;
        uint16_t age = 0, completed = 0;
    };
    DmTrace previous_dm{}, worst_dm_before{}, worst_dm_after{};
    uint64_t worst_dm_gap = 0;
    bool previous_dm_valid = false;
    std::array<Timing, 6> motor_capture_gap{}, motor_page_age{};
    std::array<uint32_t, 6> motor_capture_at{};
    std::array<uint64_t, 6> long_gaps_with_capture_gap{};
    unsigned max_hi91_pending = 0, max_hi91_pending_on_gap = 0;
    std::array<uint64_t, 6> motor_gaps_over_2ms{},
        motor_gaps_with_late_wake{};
    uint64_t missed_releases = 0;
    std::array<uint64_t, 6> motor_last_missed_releases{};
    uint64_t imu_at = 0, start = 0, deadline = 0, probe_at = 0;
    uint8_t sequence = 0, probe_sequence = 0;
    unsigned cycles = 0, wkc_failures = 0, invalid = 0, probes = 0;
    bool ready = false, protected_seen = false;
    const char* failure = "";
    std::array<uint8_t,60> invalid_image{};
    size_t invalid_offset=0;
    unsigned batches_sent = 0, batches_acknowledged = 0;
    uint32_t sensor_fault_age=0;unsigned sensor_fault_port=0,sensor_fault_length=0;
    namespace sensor = dust::device_sensor_page;
    sensor::Assembler hi91(sensor::Kind::Hi91, 20000),
        hi91_other(sensor::Kind::Hi91, 20000),
        sbus(sensor::Kind::Sbus, 20000), board_imu(sensor::Kind::Imu, 20000),
        board_imu_compact(sensor::Kind::ImuCompact, 20000);
    std::array<sensor::Complete, 2> hi91_ready{};
    uint32_t hi91_expected = 1, hi91_partial_sequence = 0;
    unsigned hi91_out_of_order = 0;
    uint64_t hi91_gap_start = 0;
    Timing hi91_gap_recovery;
    uint32_t last_write_trace = 0;
    bool have_write_trace = false;
    uint64_t unseen_writes = 0, repeated_images = 0;
    std::array<unsigned, 3> unseen_first_hi91{};
    unsigned unseen_retry_hi91 = 0, unseen_compact_hi91 = 0;
    uint32_t longest_hi91_from = 0, longest_hi91_to = 0;
    uint32_t hi91_last_device_ms = 0, longest_hi91_device_delta = 0;
    Timing hi91_device_interval;
    std::array<uint32_t, 6> hi91_last_trace{}, hi91_long_from_trace{}, hi91_long_to_trace{};
    std::array<uint8_t, 6> last_uart_key{};
    unsigned trace_mismatches = 0;
    std::array<uint32_t, 2> ack_sequence{}, uart_blocks{},
        uart_discontinuities{};
    std::array<bool, 2> have_ack{};
    std::array<uint32_t, 2> uart_skipped_blocks{};
    std::array<uint8_t, 2> ack_repeats{};
    struct SbusHostTrace {
        uint32_t sequence = 0;
        uint64_t first_rx_ns = 0, first_ack_ns = 0;
    };
    std::array<SbusHostTrace, 64> sbus_host_trace{};
    uint8_t ack_pending = 0;
    Timing hi91_assembly, hi91_ack_submit;
    ArrivalTiming hi91_frame_interval;
    uint32_t hi91_observed_sequence = 0;
    uint64_t hi91_first_fragment_at = 0, hi91_complete_at = 0;
    unsigned hi91_first_ack_submits = 0;
    uint8_t hi91_first_pass_mask = 0, hi91_retry_mask = 0;
    uint8_t hi91_partial_mask = 0;
    bool hi91_partial_dirty = false;
    std::array<unsigned, 3> hi91_missing_first_pass{}, hi91_recovered{};
    unsigned hi91_blocks_missing_first_pass = 0, hi91_complete_first_pass = 0;
    std::vector<uint8_t> hi91_bytes, sbus_bytes;
    if (sensors) {
        hi91_bytes.reserve(seconds * 100000 + 1000);
        sbus_bytes.reserve(seconds * 3000 + 1000);
    }
    const auto interval_capacity = (seconds + 1) * 8000;
    for (auto& interval : motor_interval) {
        interval.intervals.reserve(interval_capacity);
    }
    imu_interval.intervals.reserve(interval_capacity);
    hi91_frame_interval.intervals.reserve(interval_capacity);
    std::array<std::array<PhaseSample, 8>, 8> phases{};
    uint64_t phase_origin = 0, last_probe_period = UINT64_MAX;
    uint64_t hi91_sample_at = 0;
    uint32_t hi91_previous_device_ms = 0;
    uint32_t hi91_longest_device_delta_ms = 0, hi91_longest_sequence_delta = 0;
    uint32_t hi91_compact_seq = 0;
    unsigned hi91_compact_count = 0, hi91_compact_skipped = 0;
    size_t hi91_scan = 0, sbus_scan = 0;
    uint64_t sbus_frame_at = 0;
    ArrivalTiming sbus_frame_interval;
    sbus_frame_interval.intervals.reserve(interval_capacity);
    unsigned accepted_probes = 0;
    unsigned protected_reason = 0;
    uint64_t next_can_command_ns = 0;
    std::array<uint8_t, wire::kMotorBatchBytes> held_can_page{};
    bool have_held_can_page = false;
    auto send = [&](wire::Type type, bool probe) {
        std::fill(boards[0].output.begin(), boards[0].output.end(), 0);
        const auto send_now = Now();
        const bool command_due = safe_can
            ? !next_can_command_ns || send_now >= next_can_command_ns
            : !ack_pending && !hi91_partial_dirty;
        if (batch && start && type == wire::Type::Arm && !probe && command_due) {
            if (safe_can) {
                if (!next_can_command_ns) next_can_command_ns = send_now;
                next_can_command_ns += 1000000;
                if (next_can_command_ns <= send_now) next_can_command_ns = send_now + 1000000;
            }
            ++sequence;
            for (unsigned slot = 0; slot < 6; ++slot) {
                wire::Payload payload{};
                if (safe_can) {
                    if (slot != unsigned(wire::Device::RightWheel) &&
                        slot != unsigned(wire::Device::LeftWheel)) {
                        payload.fill(0xff);
                        payload[7] = 0xfd;
                    }
                } else {
                    payload[0] = uint8_t(slot + 1);
                    payload[1] = sequence;
                }
                wire::Record record{};
                wire::EncodeMotor(wire::Device(slot), sequence, payload,
                                  record);
                std::copy(record.begin(), record.end(),
                          boards[0].output.begin() + slot * 10);
            }
            std::array<wire::Payload,6> computed{};
            if(physical_live && bridge.Payloads(computed)) {
                for(unsigned slot=0;slot<6;++slot)std::copy(computed[slot].begin(),computed[slot].end(),boards[0].output.begin()+slot*10+2);
            }
            if (safe_can) {
                std::copy_n(boards[0].output.begin(), held_can_page.size(), held_can_page.begin());
                have_held_can_page = true;
            }
            ++batches_sent;
            return link->Send(boards, error);
        }
        if (safe_can && have_held_can_page && start && type == wire::Type::Arm &&
            !ack_pending && !hi91_partial_dirty) {
            std::copy(held_can_page.begin(), held_can_page.end(), boards[0].output.begin());
            return link->Send(boards, error);
        }
        token[4] = uint8_t(probe);
        token[5] = ack_pending;
        const bool first_hi91_ack =
            type == wire::Type::Arm && (ack_pending & 1U) && ack_repeats[0] == 2;
        token[6] = uint8_t(first_hi91_ack) | (disable_hi91 ? 2U : 0U) | (motors_only ? 4U : 0U) |
                   (motors_imu ? 8U : 0U) | (event_only ? 16U : 0U) |
                   ((imu_latency || combined) ? 32U : 0U) | (live_shadow ? 64U : 0U) |
                   (disable_sbus ? 128U : 0U);
        token[7] = uint8_t(uart_no_ack) | (hi91_window ? 2U : 0U) |
                   (early_retry ? 4U : 0U) | (hi91_compact ? 8U : 0U) | (dm_disable ? 16U : 0U) | (dm_stagger_slots << 5);
        const auto record = wire::Encode(type, ++sequence, token);
        std::copy(record.begin(), record.end(), boards[0].output.begin());
        if (sensors) {
            for (unsigned i = 0; i < 2; ++i) {
                sensor::Put32(boards[0].output.data() + 10 + 4 * i,
                              ack_sequence[i]);
            }
            if (hi91_partial_dirty && type == wire::Type::Arm) {
                sensor::Put32(boards[0].output.data() + 18, hi91_partial_sequence);
                boards[0].output[22] = hi91_partial_mask;
                hi91_partial_dirty = false;
            }
            ack_pending = 0;
            for(unsigned i=0;i<2;++i){
                if(ack_repeats[i]) --ack_repeats[i];
                if(ack_repeats[i]) ack_pending |= 1U << i;
            }
        }
        const bool sent = link->Send(boards, error);
        if (sent && first_hi91_ack) {
            ++hi91_first_ack_submits;
            hi91_ack_submit.Add(Now() - hi91_complete_at);
        }
        return sent;
    };
    deadline = Now();
    const auto startup = deadline;


    while (true) {
        deadline += 125000;
        timespec release{time_t(deadline / 1000000000),
                         long(deadline % 1000000000)};
        int rc;
        do {
            rc = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &release,
                                 nullptr);
        } while (rc == EINTR);
        const auto now = Now();
        if (rc || now - deadline >= 20000000) {
            failure = "host deadline";
            break;
        }
        if (start) {
            wake_lateness.Add(now - deadline);
            missed_releases += (now - deadline) / 125000;
        }
        if (now - deadline >= 125000) {
            deadline += ((now - deadline) / 125000) * 125000;
        }
        if(bridge.ShouldStop()){if(bridge.Fault())failure="controller fault";break;}
        bridge.PollRemote(now/1000);
        if (!link->Receive(boards, error)) {
            failure = "receive";
            break;
        }
        if (boards[0].operational && boards[0].complete) {
            const auto* image = boards[0].input.data();
            for (unsigned page = 0; page < (combined ? 3U : 1U); ++page) {
                if (combined && page != 0 &&
                    (image[0] == uint8_t(wire::Type::Status) ||
                     image[0] == uint8_t(wire::Type::Ready))) {
                    continue;
                }
                const auto* input = image + page * image_size;
                sensor::Page imu_page{};
                if (combined && page == 1) {
                    const auto bytes = input[0] == uint8_t(sensor::Kind::Imu)
                        ? 50 : 28;
                    std::copy_n(input, bytes, imu_page.begin());
                    input = imu_page.data();
                }
                if (!combined && start && tx_size > image_size &&
                    !std::all_of(input + image_size, input + tx_size,
                                 [tag = uint8_t(input[0] ^ input[1])](uint8_t b) {
                                     return b == tag;
                                 })) {
                    ++invalid;
                    failure = "invalid TX padding";
                    break;
                }
                if (hi91_compact && input[0] == dust::hi91::kPageType) {
                    const auto sample_sequence = sensor::Get32(input + 1);
                    if (!hi91_compact_count || sample_sequence != hi91_compact_seq) {
                        if (hi91_compact_count) {
                            const auto gap = now - hi91_sample_at;
                            const auto device_ms = uint32_t(input[57]) |
                                (uint32_t(input[58]) << 8) | (uint32_t(input[59]) << 16);
                            if (gap > hi91_frame_interval.max) {
                                hi91_longest_device_delta_ms =
                                    (device_ms - hi91_previous_device_ms) & 0xffffffU;
                                hi91_longest_sequence_delta = sample_sequence - hi91_compact_seq;
                            }
                            hi91_frame_interval.Add(gap);
                        }
                        ++hi91_compact_count;
                        hi91_compact_seq = sample_sequence;
                        hi91_previous_device_ms = uint32_t(input[57]) |
                            (uint32_t(input[58]) << 8) | (uint32_t(input[59]) << 16);
                        hi91_sample_at = now;
                        bridge.FeedImu(input+dust::hi91::kFieldsOffset,sample_sequence,now/1000);
                    }
                    continue;
                }
                if (sensors && ((input[0] >= 0x40 && input[0] <= 0x42) ||
                                input[0] == uint8_t(sensor::Kind::ImuCompact))) {
                    sensor::Fragment fragment;
                    sensor::Complete complete;
                    if (!sensor::Decode(input, image_size, fragment)) {
                        ++invalid;
                        fprintf(stderr, "invalid sensor page %u:", page);
                        for (unsigned byte = 0; byte < image_size; ++byte) {
                            fprintf(stderr, " %02x", input[byte]);
                        }
                        fprintf(stderr, "\n");
                        failure = "invalid sensor page";
                        break;
                    }
                    const bool compact_imu = fragment.kind == sensor::Kind::ImuCompact;
                    if (fragment.kind == sensor::Kind::Imu || compact_imu) {
                        auto& assembler = compact_imu ? board_imu_compact : board_imu;
                        if (assembler.Accept(input, image_size,
                                             uint32_t(now / 1000), complete)) {
                            if (imu_at) {
                                imu_interval.Add(now - imu_at);
                            }
                            last_imu_capture = complete.capture_us;
                            last_imu_sequence = complete.sequence;
                            imu_at = now;
                        }
                    } else {
                        const unsigned port =
                            fragment.kind == sensor::Kind::Hi91 ? 0 : 1;
                        auto& assembler = port == 0 ?
                            (hi91_window && (fragment.sequence & 1U) ? hi91_other : hi91)
                            : sbus;
                        if (port == 0 && !hi91_window &&
                            (!hi91_first_fragment_at ||
                             fragment.sequence != hi91_observed_sequence)) {
                            hi91_observed_sequence = fragment.sequence;
                            hi91_first_fragment_at = Now();
                            hi91_first_pass_mask = hi91_retry_mask = 0;
                            hi91_partial_mask = 0;
                            hi91_partial_dirty = false;
                        }
                        if (port == 0 && !hi91_window) {
                            const unsigned bit = 1U << (fragment.offset / sensor::kChunkBytes);
                            if (fragment.retry) {
                                hi91_retry_mask |= bit;
                            } else {
                                hi91_first_pass_mask |= bit;
                            }
                        }
                        const bool assembled = assembler.Accept(
                            input, image_size, uint32_t(now / 1000), complete);
                        if (hi91_window && port == 0) {
                            if (!hi91_gap_start &&
                                fragment.sequence == hi91_expected + 1) {
                                hi91_gap_start = Now();
                            }
                            if (assembled) {
                                hi91_ready[complete.sequence & 1U] = complete;
                                if (complete.sequence != hi91_expected) {
                                    ++hi91_out_of_order;
                                    hi91_partial_sequence = complete.sequence;
                                    hi91_partial_mask = assembler.ReceivedMask();
                                    hi91_partial_dirty = true;
                                }
                            } else if (int32_t(fragment.sequence - hi91_expected) >= 0 &&
                                       !hi91_ready[fragment.sequence & 1U].length) {
                                hi91_partial_sequence = fragment.sequence;
                                hi91_partial_mask = assembler.ReceivedMask();
                                hi91_partial_dirty = hi91_partial_mask != 0;
                            }
                        }
                        const auto deliver = [&](sensor::Complete complete) {
                            if (port == 0) {
                                hi91_partial_dirty = false;
                                hi91_complete_at = Now();
                                const auto fragments = (complete.length + sensor::kChunkBytes - 1) /
                                                       sensor::kChunkBytes;
                                const unsigned missing = hi91_window ? 0 :
                                    ((1U << fragments) - 1) & ~hi91_first_pass_mask;
                                hi91_blocks_missing_first_pass += missing != 0;
                                hi91_complete_first_pass += !hi91_window && hi91_retry_mask == 0;
                                for (unsigned i = 0; i < fragments; ++i) {
                                    hi91_missing_first_pass[i] += bool(missing & (1U << i));
                                    hi91_recovered[i] += bool(missing & hi91_retry_mask & (1U << i));
                                }
                                if (!hi91_window) {
                                    hi91_assembly.Add(hi91_complete_at - hi91_first_fragment_at);
                                }
                            }
                            if (uart_no_ack && have_ack[port]) {
                                const uint32_t skipped =
                                    complete.sequence - ack_sequence[port] - 1;
                                uart_skipped_blocks[port] += skipped;
                                complete.discontinuity |= skipped != 0;
                            }
                            auto& bytes = port == 0 ? hi91_bytes : sbus_bytes;
                            if (complete.discontinuity) {
                                (port == 0 ? hi91_scan : sbus_scan) = bytes.size();
                            }
                            bytes.insert(bytes.end(), complete.data.begin(),
                                         complete.data.begin() + complete.length);
                            if (port == 1) {
                                constexpr size_t frame_bytes =
                                    dust_control::protocols::kWflySbusFrameLength;
                                while (sbus_scan + frame_bytes <= sbus_bytes.size()) {
                                    dust_control::protocols::WflySbusFrame frame{};
                                    if (dust_control::protocols::DecodeWflySbusFrame(
                                            sbus_bytes.data() + sbus_scan, frame_bytes, &frame)) {
                                        if (sbus_frame_at) {
                                            sbus_frame_interval.Add(now - sbus_frame_at);
                                        }
                                        sbus_frame_at = now;
                                        sbus_scan += frame_bytes;
                                    } else {
                                        ++sbus_scan;
                                    }
                                }
                            }
                            if (shadow && port == 0) {
                                while (hi91_scan + 82 <= hi91_bytes.size()) {
                                    const auto* p = hi91_bytes.data() + hi91_scan;
                                    dust_control::protocols::Hi91Sample sample{};
                                    if (p[0] == 0x5a && p[1] == 0xa5 &&
                                        sensor::Get32(p + 2) % 65536 == 76 &&
                                        dust_control::protocols::DecodeHi91Frame(
                                            p, 82, true, &sample) == 0) {
                                        const auto sample_at = Now();
                                        if (hi91_sample_at) {
                                            hi91_frame_interval.Add(sample_at - hi91_sample_at);
                                        }
                                        hi91_sample_at = sample_at;
                                        hi91_scan += 82;
                                    } else {
                                        ++hi91_scan;
                                    }
                                }
                            }
                            if(port==1)bridge.FeedRemote(complete.data.data(),complete.length,complete.discontinuity,now/1000);
                            ++uart_blocks[port];
                            uart_discontinuities[port] += complete.discontinuity;
                            ack_sequence[port] = complete.sequence;
                            have_ack[port] = true;
                            if (!uart_no_ack) {
                                ack_repeats[port] = 2;
                                ack_pending |= 1U << port;
                            }
                        };
                        if (hi91_window && port == 0) {
                            auto& next = hi91_ready[hi91_expected & 1U];
                            if (next.length && next.sequence == hi91_expected) {
                                if (hi91_gap_start) {
                                    hi91_gap_recovery.Add(Now() - hi91_gap_start);
                                    hi91_gap_start = 0;
                                }
                                deliver(next);
                                next.length = 0;
                                ++hi91_expected;
                                auto& following = hi91_ready[hi91_expected & 1U];
                                if (following.length && following.sequence == hi91_expected) {
                                    deliver(following);
                                    following.length = 0;
                                    ++hi91_expected;
                                }
                            } else if (have_ack[0] &&
                                       int32_t(fragment.sequence - hi91_expected) < 0) {
                                ack_pending |= 1U;
                            }
                            if (early_retry && hi91_gap_start) {
                                auto& oldest = hi91_expected & 1U ? hi91_other : hi91;
                                hi91_partial_sequence = hi91_expected;
                                hi91_partial_mask = 0x80U | oldest.ReceivedMask(hi91_expected);
                                hi91_partial_dirty = true;
                            }
                        } else if (assembled) {
                            deliver(complete);
                        } else if (!uart_no_ack && have_ack[port] &&
                                   fragment.sequence == ack_sequence[port]) {
                            ack_pending |= 1U << port;
                        } else if (!uart_no_ack && port == 0 &&
                                   hi91_partial_mask != hi91.ReceivedMask()) {
                            hi91_partial_sequence = fragment.sequence;
                            hi91_partial_mask = hi91.ReceivedMask();
                            hi91_partial_dirty = true;
                        }
                    }
                } else {
                    for (size_t offset = 0; offset < image_size; offset += 10) {
                        const auto* data = input + offset;
                        if (batch && std::all_of(data, data + 10, [](uint8_t b) {
                                return b == 0;
                            })) {
                            continue;
                        }
                        wire::View view{};
                        if (wire::Decode(wire::Direction::Tx, data, 10, view)) {
                            const bool matching =
                                std::equal(token.begin(), token.begin() + 4,
                                           view.payload.begin());
                            if (view.type == wire::Type::Ready && matching) {
                                ready = true;
                            }
                            if (view.type == wire::Type::Status && matching) {
                                if (batch) {
                                    batches_acknowledged =
                                        unsigned(view.payload[5]) |
                                        (unsigned(view.payload[6]) << 8) |
                                        (unsigned(view.payload[7]) << 16);
                                }
                                if (view.payload[4] == 3) {
                                if(sensors){
                                    sensor_fault_port=input[10];sensor_fault_length=input[11];
                                    sensor_fault_age=sensor::Get32(input+12);
                                }
                                    protected_reason = view.payload[5];
                                    failure = "slave protected";
                                    break;
                                }
                                if (view.payload[4] == 2 && !start) {
                                    start = now;
                                    // Control worker is already running before EtherCAT startup.
                                }
                                if (sensors && view.payload[4] == 2 &&
                                    sensor::Get32(input + 10) == 0x31524354) {
                                    accepted_probes = unsigned(input[22]) |
                                                      (unsigned(input[23]) << 8);
                                    bool matched = false;
                                    for (unsigned slot = 0; slot < 6; ++slot) {
                                        const auto* trace = input + 24 + slot * 6;
                                        const auto capture = sensor::Get32(trace + 2);
                                        if (!trace[0] || !motor_at[slot] ||
                                            previous[slot] != trace[1] ||
                                            motor_capture_at[slot] != capture ||
                                            (have_trace[slot] &&
                                             last_traced_capture[slot] == capture)) {
                                            continue;
                                        }
                                        have_trace[slot] = true;
                                        last_traced_capture[slot] = capture;
                                        ++esc_matches[slot];
                                        esc_long_gap_matches[slot] += last_motor_gap_long[slot];
                                        matched = true;
                                    }
                                    if (matched) {
                                        esc_write.Add(sensor::Get32(input + 14));
                                        page_to_esc.Add(sensor::Get32(input + 18));
                                    }
                                    if (probe_at && view.sequence == probe_sequence) {
                                        rtt.Add(now - probe_at);
                                        probe_at = 0;
                                    }
                                    break;
                                }
                                if (probe_at && view.sequence == probe_sequence) {
                                    rtt.Add(now - probe_at);
                                    probe_at = 0;
                                }
                            }
                            const unsigned motor = unsigned(view.type) -
                                                   unsigned(wire::Type::MotorFirst);
                            if (start && motor < 6 &&
                                (!motor_at[motor] ||
                                 previous[motor] != view.sequence)) {
                                if (sensors && !live_shadow && view.payload[7] == 0xd1) {
                                    const auto capture = sensor::Get32(view.payload.data());
                                    const auto age = unsigned(view.payload[4]) |
                                                     (unsigned(view.payload[5]) << 8);
                                    motor_page_age[motor].Add(uint64_t(age) * 1000);
                                    const unsigned depth = view.payload[6];
                                    max_hi91_pending = std::max(max_hi91_pending, depth);
                                    if (motor_at[motor]) {
                                        const auto capture_gap = capture - motor_capture_at[motor];
                                        motor_capture_gap[motor].Add(uint64_t(capture_gap) * 1000);
                                        if (now - motor_at[motor] > 2000000) {
                                            long_gaps_with_capture_gap[motor] += capture_gap > 2000;
                                            max_hi91_pending_on_gap =
                                                std::max(max_hi91_pending_on_gap, depth);
                                        }
                                    }
                                    motor_capture_at[motor] = capture;
                                }
                                if (motor_at[motor]) {
                                    const auto gap = now - motor_at[motor];
                                    motor_interval[motor].Add(gap);
                                    last_motor_gap_long[motor] = gap > 2000000;
                                    if (last_motor_gap_long[motor]) {
                                        ++motor_gaps_over_2ms[motor];
                                        motor_gaps_with_late_wake[motor] +=
                                            missed_releases !=
                                            motor_last_missed_releases[motor];
                                    }
                                }
                                motor_last_missed_releases[motor] = missed_releases;
                                motor_at[motor] = now;
                                bridge.FeedMotor(wire::Device(motor),view.payload,now/1000);
                                previous[motor] = view.sequence;
                            }
                            if (start &&
                                imu.Accept(view, uint32_t(now / 1000), sample) &&
                                (sample[8] & 1)) {
                                if (imu_at) {
                                    imu_interval.Add(now - imu_at);
                                }
                                imu_at = now;
                            }
                        } else if (start) {
                            ++invalid;
                            failure = "invalid TX";
                            invalid_offset=offset;
                            std::copy_n(boards[0].input.begin(),image_size,invalid_image.begin());
                            break;
                        }
                    }
                }
                if (*failure) {
                    break;
                }
            }
            if (*failure) {
                break;
            }
        } else if (start) {
            ++wkc_failures;
            failure = "incomplete WKC or OP lost";
            break;
        }
        if (start && now - start >= seconds * 1000000000ULL) {
            break;
        }
        if (!start && now - startup >= 6000000000ULL) {
            failure = "handshake timeout";
            break;
        }
        bool probe = start && cycles % 40 == 0;
        if (motors_only || live_full) probe = false;
        if (probe) {
            ++probes;
            probe_sequence = uint8_t(sequence + 1);
            probe_at = Now();
        }
        if (!send(ready ? wire::Type::Arm : wire::Type::Discover, probe)) {
            failure = "send";
            break;
        }
        ++cycles;
    }
    const auto end = Now();
    bridge.Finish();
    std::array<uint32_t, 10> hi91_timing{};
    bool hi91_timing_seen = false;
    std::array<uint32_t, 14> hi91_delivery{};
    bool hi91_delivery_seen = false;
    std::array<uint32_t, 14> dm_tx{};
    std::array<uint32_t, 7> sbus_fault_trace{};
    bool sbus_fault_trace_seen = false;
    std::array<uint32_t, 3> imu_stage_counts{};
    std::array<uint32_t, 5> hi91_stage_counts{};
    bool hi91_stage_seen = false;
    bool imu_stage_seen = false;
    std::array<uint32_t, 12> gateway_can_report{};
    bool gateway_can_report_seen = false;
    bool can_source_seen = false;
    uint32_t can_source_tick_us = 0;
    std::array<uint32_t, 30> can_source{};
    std::array<uint32_t, 6> can_source_age{};
    std::array<uint32_t, 4> imu_source_arrival{};
    bool dm_tx_seen = false, dm_release_seen = false;
    std::array<uint32_t, 8> dm_release{};
    std::array<unsigned, 6> dm_driver_timing{};
    std::array<unsigned, 2> dm_prepare_us{};
    std::array<uint32_t, 12> ingress{};
    bool ingress_seen = false;
    std::array<uint32_t, 12> imu_trace{};
    bool imu_trace_seen = false;
    for (unsigned i = 0; i < 25; ++i) {
        send(wire::Type::Stop, false);
        usleep(1000);
        if (link->Receive(boards, error) && boards[0].operational &&
            boards[0].complete) {
            wire::View view{};
            const bool stopped =
                wire::Decode(wire::Direction::Tx, boards[0].input.data(), 10,
                             view) &&
                view.type == wire::Type::Status && view.payload[4] == 3;
            protected_seen |= stopped;
            const auto* input = boards[0].input.data();
            if (motors_imu && stopped && input[58] == (imu_latency ? 0xc3 : 0xc2) && !imu_trace_seen) {
                imu_trace_seen = true;
                for (unsigned i = 0; i < imu_trace.size(); ++i) {
                    imu_trace[i] = sensor::Get32(input + 10 + i * 4);
                }
            }
            if (motors_only && stopped && input[58] == 0xc1 && !ingress_seen) {
                ingress_seen = true;
                for (unsigned i = 0; i < ingress.size(); ++i) {
                    ingress[i] = sensor::Get32(input + 10 + i * 4);
                }
            }
            if (stopped && sensor::Get32(input + 10) == 0x31565241) {
                can_source_seen = true;
                can_source_tick_us = sensor::Get32(input + 14);
                for (unsigned j=0;j<4;++j) imu_source_arrival[j]=sensor::Get32(input+42+j*4);
                for (unsigned j=0;j<6;++j) can_source_age[j]=sensor::Get32(input+18+j*4);
                for (unsigned j = 0; j < can_source.size(); ++j)
                    can_source[j] = sensor::Get32(input + 60 + j * 4);
            }
            if (stopped && sensor::Get32(input + 60) == 0x314e4143) {
                gateway_can_report_seen = true;
                for (unsigned i = 0; i < gateway_can_report.size(); ++i) {
                    gateway_can_report[i] = sensor::Get32(input + 64 + i * 4);
                }
            }
            if (dm_disable && stopped && sensor::Get32(input + 120) == 0x334d4444) {
                dm_tx_seen = true;
                for (unsigned i = 0; i < dm_tx.size(); ++i) {
                    dm_tx[i] = sensor::Get32(input + 124 + i * 4);
                }
            }
            if (stopped && sensor::Get32(input + 60) == 0x31554253) {
                sbus_fault_trace_seen = true;
                for (unsigned i = 0; i < sbus_fault_trace.size(); ++i) {
                    sbus_fault_trace[i] = sensor::Get32(input + 64 + i * 4);
                }
            }
            if (dm_disable && stopped && sensor::Get32(input + 60) == 0x324d5549) {
                hi91_stage_seen = true;
                for (unsigned i = 0; i < hi91_stage_counts.size(); ++i) {
                    hi91_stage_counts[i] = sensor::Get32(input + 64 + 4 * i);
                }
            }
            if (dm_disable && stopped && (sensor::Get32(input + 60) == 0x314d5549 ||
                sensor::Get32(input + 60) == 0x324d5549)) {
                imu_stage_seen = true;
                for (unsigned i = 0; i < imu_stage_counts.size(); ++i) {
                    imu_stage_counts[i] = sensor::Get32(input + 96 + 4 * i);
                }
            }
            if (dm_disable && stopped && sensor::Get32(input + 60) == 0x374d4444) {
                dm_release_seen = true;
                for (unsigned i = 0; i < dm_release.size(); ++i) {
                    dm_release[i] = sensor::Get32(input + 64 + i * 4);
                }
                dm_prepare_us = {input[108] * 10U, input[109] * 10U};
                for (unsigned i = 0; i < dm_driver_timing.size(); ++i) {
                    dm_driver_timing[i] = input[96 + 2 * i] | (input[97 + 2 * i] << 8);
                }
            }
            if (combined && stopped && sensor::Get32(input + 60) == 0x314d4144) {
                hi91_delivery_seen = true;
                for (unsigned field = 0; field < hi91_delivery.size(); ++field) {
                    hi91_delivery[field] = sensor::Get32(input + 64 + field * 4);
                }
            }
            if (sensors && stopped && sensor::Get32(input + 16) == 0x314b4341) {
                hi91_timing_seen = true;
                for (unsigned field = 0; field < hi91_timing.size(); ++field) {
                    hi91_timing[field] = sensor::Get32(input + 20 + field * 4);
                }
            }
            if (batch && stopped) {
                batches_acknowledged = unsigned(view.payload[5]) |
                                       (unsigned(view.payload[6]) << 8) |
                                       (unsigned(view.payload[7]) << 16);
            }
        }
    }
    link->Close();
    printf("{\"cycle_us\":125,\"rx_bytes\":%zu,\"tx_bytes\":%zu,"
           "\"active_seconds\":%.6f,\"error\":\"%s\",\"wkc_failures\":%u,"
           "\"missed_releases\":%llu,\"invalid\":%u,\"stop_seen\":%s,"
           "\"motor_intervals\":[", image_size, tx_size, start ? double(end - start) / 1e9 : 0,
           failure, wkc_failures, (unsigned long long)missed_releases, invalid,
           protected_seen ? "true" : "false");
    for (unsigned i = 0; i < motor_interval.size(); ++i) {
        if (i) printf(",");
        motor_interval[i].Print();
    }
    printf("],\"imu_interval\":");
    imu_interval.Print();
    printf(",\"hi91_frame_interval\":");
    hi91_frame_interval.Print();
    printf(",\"sbus_frame_interval\":");
    sbus_frame_interval.Print();
    printf(",\"hi91_longest_device_delta_ms\":%u,\"hi91_longest_sequence_delta\":%u",
           hi91_longest_device_delta_ms, hi91_longest_sequence_delta);
    bridge.Print();
    printf(",\"safe_can\":%s,\"can_command_batches\":%u",
           physical_live ? "false" : "true", batches_sent);
    printf(",\"protected_reason\":%u", protected_reason);
    if (can_source_seen) {
        printf(",\"can_source_tick_us\":%u,\"can_source_intervals\":[",can_source_tick_us);
        for (unsigned j=0;j<6;++j) {
            const auto* a=can_source.data()+j*5;
            printf("%s{\"count\":%u,\"irq_min_us\":%u,\"irq_max_us\":%u,\"wire_min_us\":%llu,\"wire_max_us\":%llu,\"sof_to_driver_sample_max_us\":%llu}",j?",":"",a[0],a[1],a[2],
                (unsigned long long)a[3]*can_source_tick_us,(unsigned long long)a[4]*can_source_tick_us,(unsigned long long)can_source_age[j]*can_source_tick_us);
        }
        printf("]");
        printf(",\"imu_source_arrival\":{\"drdy_count\":%u,\"drdy_max_us\":%u,\"dma_count\":%u,\"dma_max_us\":%u}",imu_source_arrival[0],imu_source_arrival[1],imu_source_arrival[2],imu_source_arrival[3]);
    }
    if (gateway_can_report_seen) {
        printf(",\"can_passthrough\":[");
        for (unsigned side = 0; side < 2; ++side) {
            const auto* stat = gateway_can_report.data() + side * 6;
            printf("%s{\"bus\":%u,\"accepted\":%u,\"completed\":%u,"
                   "\"frames_completed\":%u,\"cancelled\":%u,"
                   "\"protection_completed\":%u,\"error\":%d}",
                   side ? "," : "", side ? 3 : 0, stat[0], stat[1], stat[2],
                   stat[3], stat[4], int32_t(stat[5]));
        }
        printf("]");
    }
    printf("}");
    puts("");
    return failure[0] ? 1 : 0;
}

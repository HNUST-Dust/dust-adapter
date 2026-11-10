/* SPDX-License-Identifier: Apache-2.0 */

/* SSC 栈适配器：生命周期、回调翻译、发布调度与状态路由。
 * 本文件是唯一把 SSC 全局状态/ESC 寄存器与 domain 服务连接起来的地方。 */

#include "adapters/ethercat_ssc/ssc_stack.hpp"

#include "adapters/ethercat_ssc/ssc_context.hpp"
#include "common/device_pdo.hpp"
#include "common/device_sensor_page.hpp"

#include <cerrno>
#include <cstring>

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

/* ESC 生命周期/寄存器原语由 adapters/esc 提供；这里只声明用到的入口。 */
extern "C" {
uint16_t MainInit(void);
void MainLoop(void);
void PDI_Isr(void);
void esc_set_pdi_hook(void (*hook)(void));
void esc_set_reset_hook(void (*hook)(void));
void esc_start(void);
void esc_stop(void);
void esc_update_link(void);
void esc_read(volatile void *data, uint16_t address, uint16_t length);
}
#include "common/abi.hpp"
#include "domain/protocol/wire.hpp"
#include "drivers/can/can_bus.hpp"
#include "drivers/can/can_driver.hpp"
#include "drivers/uart/uart_port.hpp"

namespace ecat {
namespace wire = dust::device_pdo;

SscContext g_ctx;

namespace {

uint64_t NowUs() {
    return k_ticks_to_us_floor64(k_uptime_ticks());
}


std::array<uint8_t, dust::device_pdo::kMotorBatchBytes> pdo_worker_rx{};
struct PdoWorkerSnapshot {
    std::array<uint8_t, 180> bytes{};
};
std::array<PdoWorkerSnapshot, 2> pdo_worker_tx{};
atomic_t pdo_worker_published;
bool pdo_worker_rx_pending;
atomic_t gateway_command_event;
uint8_t uart_submission_turn;
K_SEM_DEFINE(pdo_worker_event, 0, 1);

void RequestGatewayProtection();
void ConsumeHi91Bytes(uint32_t epoch, const uint8_t* data, size_t length,
                      uint32_t capture_us, bool discontinuity);
bool QueueMotorCommands(const uint8_t* data, uint64_t now);

void PdiHook() {
    if (g_ctx.stack_ready) {
        PDI_Isr();
    }
}

void ResetHook() {
    g_ctx.reset_requested = true;
}



}  // namespace

SscStack::SscStack(Session& session, LayoutService& layout, IoCoordinator& io,
                   ProtectionService& protection,
                   const SscBoardConfig& board)
    : session_(session),
      layout_(layout),
      io_(io),
      protection_(protection),
      board_(board) {
    g_ctx.stack = this;
    g_ctx.session = &session_;
    g_ctx.layout = &layout_;
    g_ctx.io = &io_;
    g_ctx.protection = &protection_;
    g_ctx.board = &board_;
}

int SscStack::Start() {
    UartPort::SetHi91Consumer(ConsumeHi91Bytes);
    esc_set_pdi_hook(PdiHook);
    esc_set_reset_hook(ResetHook);
    if (MainInit() != 0U) {
        return -1;
    }
    ResetRuntimeState();
    /* 启动/ESC 复位后 ECAT_Init() 重建字典；同一会话内的 rearm 只重建状态，
     * 必须保留已注册的自定义对象。 */
    if (OBJ_GetObjectHandle(0x2100) == nullptr && !SscRegisterObjects()) {
        return -1;
    }
    return ApplyLimits();
}

void SscStack::Enable() {
    g_ctx.stack_ready = true;
    esc_start();
}

void SscStack::Stop() {
    if (Active()) {
        RequestGatewayProtection();
    }
    g_ctx.stack_ready = false;
    esc_stop();
}

void SscStack::Step() {
    io_serviced_ = false;
    MainLoop();
}
void SscStack::RunPdoWorker() {
    for (;;) {
        (void)k_sem_take(&pdo_worker_event, K_MSEC(1));
        const auto key = irq_lock();
        g_ctx.new_output = pdo_worker_rx_pending;
        if (pdo_worker_rx_pending) {
            g_ctx.device_rx = pdo_worker_rx;
            pdo_worker_rx_pending = false;
        }
        irq_unlock(key);

        bool publish = g_ctx.new_output || atomic_get(&gateway_command_event) || !Active();
        GatewayStep();
        if (!publish && Active()) {
            continue;
        }
        const auto next = 1 - atomic_get(&pdo_worker_published);
        auto& snapshot = pdo_worker_tx[next];
            auto& tx = snapshot.bytes;
        std::copy(g_ctx.device_tx.begin(), g_ctx.device_tx.end(), tx.begin());
        std::copy(g_ctx.combined_imu.begin(), g_ctx.combined_imu.end(), tx.begin() + 60);
        std::copy(g_ctx.combined_uart.begin(), g_ctx.combined_uart.end(), tx.begin() + 120);
        atomic_set(&pdo_worker_published, next);
        k_yield();
    }
}


StackEvents SscStack::TakeEvents() {
    StackEvents events;
    events.reset_requested = g_ctx.reset_requested;
    events.session_stopped = g_ctx.session_stopped;
    g_ctx.reset_requested = false;
    g_ctx.session_stopped = false;
    return events;
}

AlState SscStack::Al() const {
    switch (nAlStatus & STATE_MASK) {
        case STATE_INIT:
            return AlState::Init;
        case STATE_PREOP:
            return AlState::PreOp;
        case STATE_SAFEOP:
            return AlState::SafeOp;
        case STATE_OP:
            return AlState::Op;
        default:
            return AlState::Unknown;
    }
}

bool SscStack::Active() const {
    return g_ctx.device_state == 2;
}

bool SscStack::TransportHealthy() const {
    return transport_healthy_;
}



namespace {
void ConsumeHi91Bytes(uint32_t epoch, const uint8_t* data, size_t length,
                      uint32_t capture_us, bool discontinuity) {
    const auto generation = g_ctx.hi91_session_generation;
    if (g_ctx.device_state != 2 || !g_ctx.hi91_enabled) return;
    if (g_ctx.hi91_parse_epoch != epoch ||
        g_ctx.hi91_parser_generation != generation) {
        g_ctx.hi91_stream = {};
        g_ctx.hi91_parse_epoch = epoch;
        g_ctx.hi91_parser_generation = generation;
    } else if (discontinuity) {
        g_ctx.hi91_stream.Discontinuity();
    }
    dust::hi91::Compact sample;
    for (size_t j = 0; j < length; ++j) {
        if (!g_ctx.hi91_stream.Accept(data[j], sample)) continue;
        const auto key = irq_lock();
        if (g_ctx.device_state != 2 || generation != g_ctx.hi91_session_generation) {
            irq_unlock(key);
            continue;
        }
        g_ctx.hi91_device_time_ms = g_ctx.hi91_stream.DeviceTimeMs();
        g_ctx.hi91_sample_capture = capture_us;
        ++g_ctx.hi91_sample_seq;
        g_ctx.hi91_sample_valid = true;
        auto& page = g_ctx.hi91_page;
        page.fill(0);
        page[0] = dust::hi91::kPageType;
        Put32(page.data() + 1, g_ctx.hi91_sample_seq);
        std::copy(sample.begin(), sample.end(), page.begin() + dust::hi91::kFieldsOffset);
        for (unsigned n = 0; n < 3; ++n)
            page[57 + n] = uint8_t(g_ctx.hi91_device_time_ms >> (8 * n));
        irq_unlock(key);
    }
}
}  // namespace

namespace {
K_SEM_DEFINE(can2_tx_event, 0, 1);
K_SEM_DEFINE(can3_tx_event, 0, 1);
#if defined(CONFIG_ECAT_CAN_ISOLATION)
K_SEM_DEFINE(can0_tx_event, 0, 1);
K_SEM_DEFINE(can1_tx_event, 0, 1);
k_sem* const can_tx_events[]{&can0_tx_event, &can1_tx_event, &can2_tx_event, &can3_tx_event};
#else
k_sem* const can_tx_events[]{&can2_tx_event, &can3_tx_event};
#endif

struct GatewayCanTx {
    std::array<drivers::CanFrame, 16> frames{};
    uint64_t request_us = 0;
    uint8_t count = 0, sequence = 0;
    bool pending = false;
    bool queued = false;
    bool protection = false;
    int error = 0;
};
std::array<GatewayCanTx, wire::kTxBuses.size()> gateway_can_tx{};
bool gateway_protection_requested;
uint64_t gateway_last_command_us;
uint8_t gateway_command_sequence;
bool gateway_have_command_sequence;
atomic_t gateway_can_fault;

void RequestGatewayProtection() {
    const auto key = irq_lock();
    if (gateway_protection_requested) {
        irq_unlock(key);
        return;
    }
    gateway_protection_requested = true;
    for (auto& tx : gateway_can_tx) {
        tx.protection = true;
        tx.queued = true;
    }
    irq_unlock(key);
    for (auto* event : can_tx_events) {
        k_sem_give(event);
    }
}

bool GatewayCanIdle() {
    const auto key = irq_lock();
    const bool idle = std::all_of(gateway_can_tx.begin(), gateway_can_tx.end(),
        [](const auto& tx) { return !tx.pending && !tx.queued && !tx.error; });
    irq_unlock(key);
    return idle;
}

bool QueueMotorCommands(const uint8_t* data, uint64_t now) {
    namespace wire = dust::device_pdo;
    if (gateway_have_command_sequence &&
        !wire::Newer(data[1], gateway_command_sequence)) {
        return true;
    }
    std::array<wire::MotorCommand, 6> commands;
    uint8_t sequence;
    if (!wire::DecodeMotorCommands(data, wire::kMotorBatchBytes, sequence, commands)) {
        return false;
    }
    std::array<std::array<drivers::CanFrame, 3>, wire::kTxBuses.size()> frames{};
    unsigned counts[wire::kTxBuses.size()]{};
    for (const auto& command : commands) {
        CanFrame frame;
        frame.bus = command.bus;
        frame.id = command.id;
        frame.length = 8;
        frame.data = command.payload;
        if (!g_ctx.io->ValidateGatewayCan(frame)) {
            return false;
        }
        const auto side = wire::BusLane(frame.bus);
        if (side >= frames.size() || counts[side] >= frames[side].size()) return false;
        auto& output = frames[side][counts[side]++];
        output.id = frame.id;
        output.length = frame.length;
        std::copy(frame.data.begin(), frame.data.end(), output.data);
    }
    const auto key = irq_lock();
    if (g_ctx.device_state != 2 || gateway_protection_requested) {
        irq_unlock(key);
        return false;
    }
    for (unsigned side = 0; side < wire::kTxBuses.size(); ++side) {
        if (!counts[side]) continue;
        auto& tx = gateway_can_tx[side];
        std::copy_n(frames[side].begin(), counts[side], tx.frames.begin());
        tx.count = counts[side];
        tx.sequence = sequence;
        tx.request_us = now;
        tx.protection = false;
        tx.queued = true;
    }
    gateway_last_command_us = now;
    g_ctx.device_last_rx = now;
    atomic_set(&gateway_command_event, 1);
    gateway_command_sequence = sequence;
    gateway_have_command_sequence = true;
    irq_unlock(key);
    for (auto* event : can_tx_events) {
        k_sem_give(event);
    }
    k_sem_give(&pdo_worker_event);
    return true;
}

void GatewayCanComplete(void*, uint8_t bus, int error) {
    const auto lane = wire::BusLane(bus);
    if (lane >= gateway_can_tx.size()) return;
    auto& tx = gateway_can_tx[lane];
    tx.pending = false;
    if (error && error != -ECANCELED) {
        tx.error = error;
        atomic_set(&gateway_can_fault, 1);
        k_sem_give(&pdo_worker_event);
    }
    if (tx.queued) {
        k_sem_give(can_tx_events[lane]);
    }
}
}

void SscStack::RunCanTransmit(unsigned side) {
    const uint8_t bus = wire::kTxBuses[side];
    auto& tx = gateway_can_tx[side];
    auto wait = K_FOREVER;
    for (;;) {
        (void)k_sem_take(can_tx_events[side], wait);
        wait = K_FOREVER;
        const auto key = irq_lock();
        if (!tx.queued || (tx.pending && !tx.protection)) {
            irq_unlock(key);
            continue;
        }
        const bool protection = tx.protection;
        const uint64_t request = tx.request_us;
        const uint8_t command_sequence = tx.sequence;
        std::array<drivers::CanFrame, 16> frames = tx.frames;
        size_t count = tx.count;
        tx.queued = false;
        irq_unlock(key);

        if (protection) {
            const int aborted = drivers::CanAbortTx(bus);
            if (aborted) {
                drivers::CanStopController(bus);
                tx.error = aborted;
                atomic_set(&gateway_can_fault, 1);
                continue;
            }
            std::array<CanFrame, 16> configured;
            const int copied = io_.CopyGatewayProtection(bus, configured.data(), configured.size());
            if (copied < 0) {
                drivers::CanStopController(bus);
                tx.error = copied;
                atomic_set(&gateway_can_fault, 1);
                continue;
            }
            count = size_t(copied);
            for (size_t i = 0; i < count; ++i) {
                frames[i].id = configured[i].id;
                frames[i].length = configured[i].length;
                std::copy(configured[i].data.begin(), configured[i].data.end(), frames[i].data);
            }
        }
        if (!count) {
            continue;
        }
        const auto send_key = irq_lock();
        if (!protection && (!Active() || gateway_protection_requested ||
                            NowUs() >= request + 1000)) {
            irq_unlock(send_key);
            continue;
        }
        tx.pending = true;
        irq_unlock(send_key);
        struct Admission { unsigned side; uint64_t request; uint8_t sequence; }
            admission{side, request, command_sequence};
        const auto admit = +[](void* context) {
            const auto& value = *static_cast<Admission*>(context);
            const auto& current = gateway_can_tx[value.side];
            return g_ctx.device_state == 2 && !gateway_protection_requested &&
                !atomic_get(&gateway_can_fault) && !current.protection &&
                current.request_us == value.request && current.sequence == value.sequence;
        };
        const int rc = drivers::CanSendBatch(bus, frames.data(), count,
            protection ? 0 : request + 1000, GatewayCanComplete, nullptr,
            protection ? nullptr : admit, &admission);
        const auto result_key = irq_lock();
        if (rc) {
            tx.pending = false;
            if (rc == -EAGAIN || rc == -EBUSY) {
                if (!tx.queued && (protection || !gateway_protection_requested)) {
                    tx.queued = true;
                    tx.protection = protection;
                    tx.request_us = request;
                    tx.count = uint8_t(count);
                    tx.frames = frames;
                    wait = K_USEC(125);
                }
            } else if (rc != -ECANCELED) {
                tx.error = rc;
                atomic_set(&gateway_can_fault, 1);
                k_sem_give(&pdo_worker_event);
            }
        }
        irq_unlock(result_key);
    }
}

void SscStack::Poll(bool commit_tx) {
    (void)commit_tx;
    if (!g_ctx.session) {
        return;
    }
    uint8_t watchdog = 0;
    {

        esc_read(&watchdog, 0x0440, 1);
    }
    const uint64_t now = NowUs();
    transport_healthy_ = bEcatOutputUpdateRunning && (watchdog & 1) != 0;
    session_.Tick(now, transport_healthy_);
    /* Application 已服务本轮 IO 时不重复 Pump；无应用回调时仍处理保护帧。 */
    if (!io_serviced_) {
        io_.Service(session_, now);
        io_serviced_ = true;
    }
    /* PDO 只由 SSC 的一次 InputMapping 提交；CAN 在 Application 和映射前刷新。 */

    static int64_t next_link_poll = 0;
    const int64_t now_ms = k_uptime_get();
    if (now_ms >= next_link_poll) {
        esc_update_link();
        next_link_poll = now_ms + 100;
    }
}

uint8_t SscStack::ReadObject(uint16_t index, uint8_t subindex,
                             uint32_t size, uint16_t* data, uint8_t complete) {
    return SscRead(index, subindex, size, data, complete);
}

uint8_t SscStack::WriteObject(uint16_t index, uint8_t subindex,
                              uint32_t size, uint16_t* data,
                              uint8_t complete) {
    return SscWrite(index, subindex, size, data, complete);
}

uint16_t SscStack::GenerateMapping(uint16_t* input, uint16_t* output) {
    *input = 0;
    *output = 0;
    g_ctx.mapping_ready = false;
    const Error commit = layout_.GenerateMapping(g_ctx.limits);
    if (commit != Error::None ||
        !io_.CanCapacityValid(layout_.transaction().candidate())) {
        printk("Gateway: mapping rejected: commit=%u\n", unsigned(commit));
        return ALSTATUSCODE_INVALIDOUTPUTMAPPING;
    }
    g_ctx.data_entry_count = 0;
    for (unsigned i = 0; i < g_ctx.object_count; ++i) {
        auto& object = g_ctx.objects[i];
        if (object.descriptor.Index >= 0x2000 &&
            object.descriptor.Index != 0x2100) {
            object.count = 0;
            object.descriptor.ObjDesc.ObjFlags = OBJCODE_REC << 8;
            object.descriptor.pEntryDesc = &g_ctx.empty_entry;
        }
    }
    if (!SscDescribe(layout_.physical_rx(), false) ||
        !SscDescribe(layout_.physical_tx(), true)) {
        return ALSTATUSCODE_INVALIDOUTPUTMAPPING;
    }
    *input = layout_.physical_tx().bytes;
    *output = layout_.physical_rx().bytes;
    sSyncManOutPar.u16SyncType = SYNCTYPE_SM_SYNCHRON;
    sSyncManInPar.u16SyncType = SYNCTYPE_SM2_SYNCHRON;
    bSyncSetByUser = TRUE;
    g_ctx.mapping_ready = true;
    return ALSTATUSCODE_NOERROR;
}

uint16_t SscStack::StartInput(uint16_t*) {
    const bool freerun = sSyncManOutPar.u16SyncType == SYNCTYPE_FREERUN &&
                         sSyncManInPar.u16SyncType == SYNCTYPE_FREERUN;
    if (!g_ctx.mapping_ready) {
        printk("Gateway: SAFEOP rejected: mapping not ready\n");
        return ALSTATUSCODE_INVALIDOUTPUTMAPPING;
    }
    uint8_t sm[16]{};
    esc_read(sm, 0x810, sizeof(sm));
    const bool sm_ok =
        U16(sm) == board_.rx_address &&
        U16(sm + 2) == layout_.physical_rx().bytes &&
        (sm[4] & 0x47) == 0x44 && (sm[6] & 1) &&
        U16(sm + 8) == board_.tx_address &&
        U16(sm + 10) == layout_.physical_tx().bytes && (sm[12] & 0x07) == 0 &&
        (sm[14] & 1);
    bool sync_ok = freerun;
    sync_ok = sync_ok ||
              (sSyncManOutPar.u16SyncType == SYNCTYPE_SM_SYNCHRON &&
               sSyncManInPar.u16SyncType == SYNCTYPE_SM2_SYNCHRON);
    if (!sm_ok || !sync_ok) {
        printk("Gateway: SAFEOP rejected: SM/sync mismatch\n");
        return ALSTATUSCODE_INVALIDOUTPUTMAPPING;
    }
    io_.Configure(layout_.transaction().candidate());
    layout_.Freeze();
    return ALSTATUSCODE_NOERROR;
}

uint16_t SscStack::StopInput() {
    if (Active()) RequestGatewayProtection();
    if (g_ctx.session &&
        g_ctx.session->state() != BusinessState::Unconfigured) {
        protection_.Report(Error::Transport);
        g_ctx.session_stopped = true;
    }
    g_ctx.session_stopped = true;
    g_ctx.mapping_ready = false;
    g_ctx.new_output = false;
    g_ctx.rx_image.fill(0);
    g_ctx.tx_image.fill(0);
    return ALSTATUSCODE_NOERROR;
}

uint16_t SscStack::StartOutput() {
    return g_ctx.mapping_ready && g_ctx.device_state == 1
        ? ALSTATUSCODE_NOERROR : ALSTATUSCODE_INVALIDOUTPUTMAPPING;
}

uint16_t SscStack::StopOutput() {
    if (Active()) {
        RequestGatewayProtection();
    }
    g_ctx.device_state=3;
    if (g_ctx.session &&
        g_ctx.session->state() == BusinessState::Active) {
        protection_.Report(Error::Transport);
    }
    g_ctx.new_output = false;
    return ALSTATUSCODE_NOERROR;
}

void SscStack::OutputMapping(uint16_t* data) {
    esc_pdo_output_received();
    if (!g_ctx.mapping_ready) {
        return;
    }
    using dust::device_pdo::Type;
    const auto* incoming = reinterpret_cast<const uint8_t*>(data);
    const auto priority = [](const uint8_t* record) {
        if (record[0] == uint8_t(Type::Stop)) {
            return 2;
        }
        return record[0] == uint8_t(Type::Arm) && record[7] != 0 ? 1 : 0;
    };
    if (incoming[0] == uint8_t(Type::MotorFirst)) {
        if (g_ctx.device_state == 2 && !QueueMotorCommands(incoming, NowUs())) {
            g_ctx.gateway_fault_reason = 1;
            g_ctx.device_state = 3;
            RequestGatewayProtection();
        }
        return;
    }
    // Keep control acknowledgments and Stop across ordinary PDO coalescing.
    if (pdo_worker_rx_pending && priority(incoming) < priority(pdo_worker_rx.data())) {
        return;
    }
    std::memcpy(pdo_worker_rx.data(), data, pdo_worker_rx.size());
    pdo_worker_rx_pending = true;
    k_sem_give(&pdo_worker_event);
    return;
}

void SscStack::InputMapping(uint16_t* data) {
    esc_pdo_mapping_state(Active());
    if (!g_ctx.mapping_ready) {
        return;
    }
    const auto& snapshot = pdo_worker_tx[atomic_get(&pdo_worker_published)];
    const auto length = layout_.physical_tx().bytes;
    if (length == 180) {
        auto* tx = reinterpret_cast<uint8_t*>(data);
        if (!Active() || snapshot.bytes[0] == uint8_t(wire::Type::Status) ||
            snapshot.bytes[0] == uint8_t(wire::Type::Ready)) {
            std::memcpy(data, snapshot.bytes.data(), length);
            return;
        }
        // Merge complete latest device sections into the fixed PDO image.
        std::copy_n(snapshot.bytes.begin(), 60, tx);
        CopyLatestMotorRecords(tx, uint32_t(NowUs()));
        std::memset(tx + 88, 0, 32);
        const auto key = irq_lock();
        const bool imu_available = io_.CopyImuPage(tx + 60);
        irq_unlock(key);
        if (!imu_available) {
            std::copy_n(snapshot.bytes.begin() + 60, 28, tx + 60);
        }
        const bool sbus_page = snapshot.bytes[120] ==
            uint8_t(dust::device_sensor_page::Kind::Sbus);
        const bool sbus_turn = sbus_page &&
            (!esc_sbus_first_write_us(U32(snapshot.bytes.data() + 122)) ||
             (uart_submission_turn & 7U) == 0);
        ++uart_submission_turn;
        bool hi91_available = false;
        if (g_ctx.hi91_enabled && !sbus_turn) {
            const auto hi91_key = irq_lock();
            hi91_available = g_ctx.hi91_sample_valid &&
                uint32_t(NowUs() - g_ctx.hi91_sample_capture) < kTimeoutUs;
            if (hi91_available) {
                std::copy(g_ctx.hi91_page.begin(), g_ctx.hi91_page.end(), tx + 120);

            }
            irq_unlock(hi91_key);
        }
        if (!hi91_available) {
            std::copy_n(snapshot.bytes.begin() + 120, 60, tx + 120);
        }
        std::memset(tx + 110, 0, 10);
        return;
    }
    std::memcpy(data, snapshot.bytes.data(), g_ctx.device_tx.size());
    if (length > g_ctx.device_tx.size()) {
        std::memset(reinterpret_cast<uint8_t*>(data) + g_ctx.device_tx.size(),
                    snapshot.bytes[0] ^ snapshot.bytes[1], length - g_ctx.device_tx.size());
    }
    return;
}




void SscStack::GatewayStatus() {
#if defined(CONFIG_ECAT_SOURCE_ARRIVAL_STATS)
    uint32_t words[36]{}, imu_words[4]{};
    if (g_ctx.device_state == 3) {
        // End the measurement before Stop emits extra protection frames.
        FreezeMotorArrivalTest(words);
        drivers::ImuSensor::FreezeArrivalTest(imu_words);
    }
#endif
    if (g_ctx.device_state == 3) {
        RequestGatewayProtection();
    }
    wire::Payload control{};
    Put32(control.data(), g_ctx.device_session);
    control[4] = g_ctx.device_state;
    control[5] = g_ctx.gateway_fault_reason;
    const auto record = wire::Encode(
        g_ctx.device_state == 1 ? wire::Type::Ready : wire::Type::Status,
        g_ctx.device_sequence, control);
    g_ctx.device_tx.fill(0);
    std::copy(record.begin(), record.end(), g_ctx.device_tx.begin());
    g_ctx.combined_imu.fill(0);
    g_ctx.combined_uart.fill(0);
#if defined(CONFIG_ECAT_SOURCE_ARRIVAL_STATS)
    if (g_ctx.device_state == 3) {
        Put32(g_ctx.device_tx.data() + 10, 0x31565241);
        Put32(g_ctx.device_tx.data() + 14, 16);
        for (unsigned i=0;i<4;++i) Put32(g_ctx.device_tx.data()+42+i*4,imu_words[i]);
        for (unsigned i = 0; i < 6; ++i)
            Put32(g_ctx.device_tx.data() + 18 + i * 4, words[30 + i]);
        for (unsigned i = 0; i < 15; ++i) {
            Put32(g_ctx.combined_imu.data() + i * 4, words[i]);
            Put32(g_ctx.combined_uart.data() + i * 4, words[i + 15]);
        }
    }
#endif
}

void SscStack::GatewayStep() {
    namespace sensor = dust::device_sensor_page;
    if (!g_ctx.mapping_ready) {
        return;
    }
    const auto lease_key = irq_lock();
    const auto now = NowUs();
    const auto last_command = gateway_last_command_us;
    const auto last_output = g_ctx.device_last_rx;
    irq_unlock(lease_key);
    if (g_ctx.device_state == 2 &&
        (!bEcatOutputUpdateRunning || atomic_get(&gateway_can_fault) ||
         (now >= last_output && now - last_output >= kTimeoutUs) ||
         (last_command && now >= last_command && now - last_command >= kTimeoutUs))) {
        g_ctx.gateway_fault_reason = !bEcatOutputUpdateRunning ? 2 :
            atomic_get(&gateway_can_fault) ? 3 :
            (now >= last_output && now - last_output >= kTimeoutUs) ? 4 : 5;
        g_ctx.device_state = 3;
    }
    auto& offer = g_ctx.sbus_offer;

    bool advance = atomic_cas(&gateway_command_event, 1, 0);
    if (g_ctx.new_output) {
        g_ctx.new_output = false;
        wire::View control{};
        const auto& rx = g_ctx.device_rx;
        const bool valid = std::all_of(rx.begin() + 23, rx.end(),
                                      [](uint8_t byte) { return byte == 0; }) &&
            wire::Decode(wire::Direction::Rx, rx.data(), wire::kBytes, control);
        if (valid) {
            const auto token = U32(control.payload.data());
            if (control.type == wire::Type::Stop) {
                g_ctx.device_state = 3;
            }
            if (control.type == wire::Type::Discover && g_ctx.device_state == 1 && token) {
                g_ctx.device_session = token;
                g_ctx.device_sequence = control.sequence;
            }
            // ACK identity belongs to the retained UART block, independently of
            // the coalesced control-page sequence. A matching session/block ACK
            // proves reception even if a newer heartbeat was already accepted.
            if (control.type == wire::Type::Arm && token &&
                token == g_ctx.device_session && g_ctx.device_state == 2 &&
                (control.payload[5] & 2U) && offer.block.length &&
                U32(rx.data() + 14) == offer.sequence) {
                offer.block.length = 0;
                g_ctx.combined_uart.fill(0);
            }
            if (control.type == wire::Type::Arm && token &&
                token == g_ctx.device_session && bEcatOutputUpdateRunning &&
                (g_ctx.device_state == 1 || g_ctx.device_state == 2) &&
                wire::Newer(control.sequence, g_ctx.device_sequence)) {
                if (g_ctx.device_state == 1) {
                    if (!GatewayCanIdle()) {
                        return;
                    }
                    g_ctx.hi91_enabled = !(control.payload[6] & 2U);
                    g_ctx.sbus_enabled = !(control.payload[6] & 128U);
                    gateway_protection_requested = false;
                    g_ctx.gateway_fault_reason = 0;
                    gateway_last_command_us = 0;
                    gateway_have_command_sequence = false;
                    atomic_clear(&gateway_can_fault);
                    ++g_ctx.hi91_session_generation;
                    g_ctx.hi91_sample_seq = 0;
                    g_ctx.hi91_sample_valid = false;
                    esc_sbus_write_reset();
                    g_ctx.combined_imu.fill(0);
                    g_ctx.combined_uart.fill(0);
                    g_ctx.device_echo = true;
#if defined(CONFIG_ECAT_SOURCE_ARRIVAL_STATS)
                    BeginMotorArrivalTest();
                    drivers::ImuSensor::BeginArrivalTest();
#endif
                    io_.StartGatewayUart();
                }
                advance = true;
                g_ctx.device_echo |= control.payload[4] == 1;
                const auto rx_key = irq_lock();
                g_ctx.device_sequence = control.sequence;
                g_ctx.device_last_rx = NowUs();
                g_ctx.device_state = 2;
                irq_unlock(rx_key);
            }
        }
    }
    // Hold the Arm acknowledgement until the first validated motor command
    // proves that the host observed it. A one-worker-pass status can disappear
    // between two 125 us EtherCAT reads when scheduling becomes faster.
    if (g_ctx.device_state != 2 || g_ctx.device_echo ||
        !gateway_have_command_sequence) {
        g_ctx.device_echo = false;
        GatewayStatus();
        return;
    }
    if (!advance) {
        return;
    }
    g_ctx.device_tx.fill(0);
    g_ctx.combined_imu.fill(0);
    // Keep the offered SBUS page visible until its matching ACK.
    // A one-worker-pass retry can miss every fixed InputMapping retry slot.
    if (!g_ctx.sbus_enabled) {
        return;
    }
    if (!offer.block.length) {
        UartRxBlock block;
        if (io_.TakeGatewayUart(4, block)) {
            offer.block = block;
            ++offer.sequence;
        }
    }
    if (!offer.block.length) {
        return;
    }
    const auto age = uint32_t(NowUs()) - offer.block.capture_us;
    if (age >= kTimeoutUs) {
        g_ctx.gateway_fault_reason = 6;
        g_ctx.device_state = 3;
        GatewayStatus();
        return;
    }
    // Encode a new block once; InputMapping retries the retained immutable page.
    if (!g_ctx.combined_uart[0]) {
        if (!sensor::Encode(sensor::Kind::Sbus, offer.sequence, offer.block.capture_us,
                            offer.block.discontinuity, offer.block.data.data(),
                            offer.block.length, 0, g_ctx.combined_uart)) {
            g_ctx.gateway_fault_reason = 7;
            g_ctx.device_state = 3;
            GatewayStatus();
            return;
        }
    }
}




void SscStack::Application() {
    // OutputMapping wakes the sole PDO owner; the SM ISR only maps completed pages.
}








void SscStack::AckError() {}

int SscStack::ApplyLimits() {
    uint8_t ram_kib = 0;
    esc_read(&ram_kib, 0x0006, 1);
    const uint32_t ram_end = 0x1000U + uint32_t(ram_kib) * 1024U;
    if (ram_end < board_.ram_end) {
        printk("Gateway: ESC RAM %u KiB cannot fit SII partitions\n", ram_kib);
        return -1;
    }
    g_ctx.limits = {board_.rx_max, board_.tx_max,
                    board_.ram_end - board_.rx_address, 0, 0, 0, false};
    io_.ApplyLimits(g_ctx.limits);
    if (board_.rx_address + 3U * g_ctx.limits.rx_bytes > board_.tx_address ||
        board_.tx_address + 3U * g_ctx.limits.tx_bytes > ram_end) {
        return -1;
    }
    printk("DTC1 LAB dictionary ready; RAM=%u KiB, Rx<=%u Tx<=%u\n", ram_kib,
           g_ctx.limits.rx_bytes, g_ctx.limits.tx_bytes);
    return 0;
}

void SscStack::ResetRuntimeState() {
    const auto key = irq_lock();
    g_ctx.mapping_ready = false;
    g_ctx.device_state = 1;
    g_ctx.device_session = 0;
    g_ctx.device_sequence = 0;
    g_ctx.device_echo = false;
    g_ctx.device_last_rx = 0;
    ++g_ctx.hi91_session_generation;
    g_ctx.hi91_sample_valid = false;
    pdo_worker_rx_pending = false;
    atomic_clear(&gateway_command_event);
    pdo_worker_tx = {};
    atomic_clear(&pdo_worker_published);
    uart_submission_turn = 0;
    irq_unlock(key);

    g_ctx.last_publish = 0;
    g_ctx.published_state = BusinessState::Unconfigured;
    g_ctx.parameters.fill(0);
    g_ctx.rx_image.fill(0);
    g_ctx.tx_image.fill(0);
    g_ctx.feedback = {};
    g_ctx.actions = {};
    g_ctx.device_rx = {};
    g_ctx.device_tx = {};
    g_ctx.combined_imu = {};
    g_ctx.combined_uart = {};
    g_ctx.sbus_offer = {};
    g_ctx.new_output = false;
    g_ctx.session_stopped = false;
    g_ctx.reset_requested = false;
    g_ctx.data_entry_count = 0;
    layout_.Reset();
}


}  // namespace ecat

/* SPDX-License-Identifier: Apache-2.0 */
#include <algorithm>
#include <drivers/can/hpmicro_mcan_batch.h>

#include <array>
#include <cstdio>
#include <string>

#include "ethercat/io_policy.hpp"
#include "io_backend.hpp"
#include "platform.hpp"
using namespace dust::ecat;
uint64_t native_now = 1000;
static device devices[10] = {{0}, {1}, {2}, {3}, {4}, {5}, {6}, {7}, {8}, {9}};
const device* NativeDevice(const char* name) {
    const std::string n(name);
    return &devices[n.find("can") != std::string::npos ? n.back() - '0'
                                                       : 3 + n.back() - '0'];
}
struct FakeCan {
    can_rx_callback_t rx{};
    void* rx_user{};
    can_tx_callback_t tx{};
    void* tx_user{};
    std::vector<can_frame> frames;
    unsigned batches = 0, aborts = 0;
};
static FakeCan cans[4];
struct FakeUart {
    uart_callback_t callback{};
    void* user{};
    std::vector<uint8_t> output;
    bool busy = false;
};
static FakeUart uarts[6];
static bool delay_admission = false;
int can_set_mode(const device*, int) {
    return 0;
}
int can_set_bitrate(const device*, uint32_t) {
    return 0;
}
int can_add_rx_filter(const device* d, can_rx_callback_t cb, void* user,
                      const can_filter*) {
    cans[d->index].rx = cb;
    cans[d->index].rx_user = user;
    return 0;
}
int can_start(const device*) {
    return 0;
}
int can_stop(const device*) {
    return 0;
}
int can_get_state(const device*, can_state* s, can_bus_err_cnt*) {
    *s = CAN_STATE_ERROR_ACTIVE;
    return 0;
}
extern "C" int hpm_mcan_send_batch(const device* d, const can_frame* f,
                                   size_t n, k_timeout_t, can_tx_callback_t cb,
                                   void* user) {
    auto& c = cans[d->index];
    if (c.tx) {
        return -EAGAIN;
    }
    c.frames.assign(f, f + n);
    ++c.batches;
    c.tx = cb;
    c.tx_user = user;
    return 0;
}
extern "C" int hpm_mcan_send_batch_until(const device* d, const can_frame* f,
                                         size_t n, k_timeout_t t,
                                         uint64_t deadline,
                                         can_tx_callback_t cb, void* user) {
    if (delay_admission) {
        native_now = deadline;
    }
    if (native_now >= deadline) {
        return -ETIMEDOUT;
    }
    return hpm_mcan_send_batch(d, f, n, t, cb, user);
}
extern "C" int hpm_mcan_abort_tx(const device* d, uint32_t) {
    auto& c = cans[d->index];
    ++c.aborts;
    if (c.tx) {
        auto cb = c.tx;
        c.tx = nullptr;
        cb(d, -ECANCELED, c.tx_user);
    }
    return 0;
}
static void CompleteCan() {
    for (unsigned i = 0; i < 4; ++i) {
        auto& c = cans[i];
        if (c.tx) {
            auto cb = c.tx;
            c.tx = nullptr;
            cb(&devices[i], 0, c.tx_user);
        }
    }
}
int uart_callback_set(const device* d, uart_callback_t cb, void* u) {
    uarts[d->index - 4].callback = cb;
    uarts[d->index - 4].user = u;
    return 0;
}
int uart_rx_enable(const device*, uint8_t*, size_t, int32_t t) {
    assert(t > 0 && (uint64_t(921600) * t + 999999) / 1000000 <= 255);
    return 0;
}
int uart_rx_disable(const device*) {
    return 0;
}
int uart_rx_buf_rsp(const device*, uint8_t*, size_t) {
    return 0;
}
int uart_config_get(const device*, uart_config*) {
    return 0;
}
int uart_configure(const device* d, const uart_config* c) {
    assert(d->index == 8 && c->parity == UART_CFG_PARITY_EVEN &&
           c->stop_bits == UART_CFG_STOP_BITS_2);
    return 0;
}
int uart_tx(const device* d, const uint8_t* data, size_t n, int32_t) {
    auto& p = uarts[d->index - 4];
    assert(!p.busy);
    p.output.assign(data, data + n);
    p.busy = true;
    return 0;
}
int uart_tx_abort(const device* d) {
    auto& p = uarts[d->index - 4];
    p.busy = false;
    uart_event e{};
    e.type = UART_TX_ABORTED;
    p.callback(d, &e, p.user);
    return 0;
}
int uart_irq_tx_complete(const device* d) {
    return !uarts[d->index - 4].busy;
}
namespace dust::ecat {
int AllocateBootIdentity(uint64_t& boot) {
    assert(cans[2].batches && cans[3].batches);
    static uint64_t counter = 100;
    boot = ++counter;
    return 0;
}
}  // namespace dust::ecat
static void Stage(ConfigurationTransaction& t, const Configuration& c) {
    const uint32_t v[] = {
        c.signature,        c.cycle_us,         c.watchdog_us,
        c.can_tx[0],        c.can_tx[1],        c.can_rx[0],
        c.can_rx[1],        c.can_rx[2],        c.can_rx[3],
        c.uart[0].tx_bytes, c.uart[0].rx_bytes, c.uart[1].tx_bytes,
        c.uart[1].rx_bytes, c.uart[2].tx_bytes, c.uart[2].rx_bytes,
        c.uart[3].tx_bytes, c.uart[3].rx_bytes, c.uart[4].tx_bytes,
        c.uart[4].rx_bytes, c.uart[5].tx_bytes, c.uart[5].rx_bytes,
        uint32_t(c.imu)};
    for (unsigned i = 0; i < 22; ++i) {
        assert(t.Write(i + 1, v[i]) == Error::None);
    }
}
struct Fixture {
    uint64_t boot{};
    Core* core;
    Configuration config;
    Layout layout;
    Limits limits{4096, 4096, 24576, 15, 63, 63, false};
    std::vector<uint8_t> rx, tx;
    Actions actions{};
    Fixture() {
        assert(InitializeIo(boot) == 0);
        CompleteCan();
        ApplyIoLimits(limits);
        assert(limits.uart_tx_mask == 47 && limits.uart_rx_mask == 63);
        config.can_tx = {1, 0};
        config.can_rx = {0, 0, 2, 0};
        config.uart[0] = {8, 16};
        assert(BuildLayout(config, limits, layout) == Error::None);
        config.signature = layout.signature;
        core = new Core(boot, IoProtection());
        ConfigurationTransaction t;
        Stage(t, config);
        assert(core->Configure(t, limits, layout.rx, layout.tx) == Error::None);
        ConfigureIo(config);
        rx.resize(layout.rx.bytes);
        tx.resize(layout.tx.bytes);
    }
    ~Fixture() {
        core->Protect(Error::State);
        ServiceIo(*core, native_now);
        CompleteCan();
        delete core;
    }
    void Header(uint32_t exchange, uint32_t sources = 1) {
        std::fill(rx.begin(), rx.end(), 0);
        Put32(rx.data(), kMagic);
        Put16(rx.data() + 4, 1);
        Put64(rx.data() + 8, 9);
        Put64(rx.data() + 16, boot);
        Put32(rx.data() + 24, exchange);
        Put32(rx.data() + 28, kArm);
        Put32(rx.data() + 32, layout.signature);
        Put32(rx.data() + 36, sources);
        Put32(rx.data() + 40, 1000);
    }
    void Can(uint32_t seq) {
        auto* p = rx.data() + layout.source[0];
        Put32(p, seq);
        Put32(p + 4, 10000);
        Put16(p + 8, 1);
        p[10] = 1;
        p[16] = 2;
        p[17] = 8;
        Put32(p + 20, 0x200);
        p[24] = 0x42;
    }
    void Uart(uint32_t seq) {
        auto* p = rx.data() + layout.uart_rx[0];
        Put32(p, seq);
        Put16(p + 12, 3);
        Put16(p + 14, kDataValid);
        std::memcpy(p + 16, "abc", 3);
    }
    void Process() {
        assert(core->Process({rx.data(), rx.size()}, native_now, true,
                             actions) == Error::None);
        DispatchIo(*core, actions, native_now);
    }
};
int main() {
    uint64_t counter = 0, boot = 0;
    assert(!NextBootIdentity(0, 0, counter, boot));
    assert(!NextBootIdentity(1, UINT64_MAX, counter, boot));
    assert(NextBootIdentity(1, 0, counter, boot) && counter == 2 && boot == 3);
    IoQueue<int, 2> queue;
    assert(queue.Push(1) && queue.Push(2) && !queue.Push(3));
    queue.Pop();
    assert(queue.Push(3) && *queue.At(0) == 2 && *queue.At(1) == 3);
    queue.Clear();
    assert(!queue.Front());
    {
        Fixture f;
        f.Header(1);
        f.Can(1);
        f.Uart(1);
        f.Process();
        assert(f.core->state() == BusinessState::Active);
        const auto batches = cans[2].batches;
        ServiceIo(*f.core, native_now);
        assert(cans[2].batches == batches + 1 && cans[2].frames.size() == 1 &&
               cans[2].frames[0].data[0] == 0x42);
        assert(uarts[0].output == std::vector<uint8_t>({'a', 'b', 'c'}));
        // 重复交换不产生第二次外设写入。
        f.Process();
        ServiceIo(*f.core, native_now);
        assert(cans[2].batches == batches + 1);
        // 最新 CAN 窗口保留最后两帧，UART 原生 idle 块按 PDO 容量分片。
        // 过期启动反馈不能进入本轮 PDO（覆盖采样时钟回绕）。
        const auto saved_now = native_now;
        native_now -= 20000;
        can_frame stale{};
        stale.id = 0x201;
        stale.dlc = 8;
        cans[2].rx(&devices[2], &stale, cans[2].rx_user);
        native_now = saved_now;
        Core::Feedback expired_feedback;
        CaptureIo(*f.core, expired_feedback, native_now);
        assert(expired_feedback.can[2].count == 0 &&
               expired_feedback.can[2].drops >= 1);
        for (unsigned i = 0; i < 3; ++i) {
            can_frame frame{};
            frame.id = 0x201;
            frame.dlc = 8;
            frame.data[0] = i;
            cans[2].rx(&devices[2], &frame, cans[2].rx_user);
        }
        const auto cycle_started = native_now;
        // 模拟周期开始后中断收到 UART 块：旧周期时间不能被当作采样上界。
        native_now += 1;
        uint8_t bytes[32]{};
        uart_event e{};
        e.type = UART_RX_RDY;
        e.data.rx = {bytes, 0, 32};
        uarts[0].callback(&devices[4], &e, uarts[0].user);
        Core::Feedback feedback;
        // CAN can be drained and exposed without consuming queued UART input.
        assert(ServiceCanReceive());
        CaptureCanFeedback(feedback);
        assert(feedback.can[2].count == 2);
        assert(!ServiceCanReceive());
        assert(f.core->Publish({f.tx.data(), f.tx.size()}, native_now,
                               feedback) == Error::None);
        assert(U16(f.tx.data() + f.layout.uart_tx[0] + 12) == 0);
        CaptureIo(*f.core, feedback, cycle_started);
        assert(f.core->state() == BusinessState::Active);
        assert(feedback.can[2].count == 2 &&
               feedback.can[2].frames[0].data[0] == 1 &&
               feedback.can[2].frames[1].data[0] == 2);
        const auto retained_batch = feedback.can[2].sequence;
        CaptureIo(*f.core, feedback, native_now);
        assert(feedback.can[2].count == 2 &&
               feedback.can[2].sequence == retained_batch);
        assert(f.core->Publish({f.tx.data(), f.tx.size()}, native_now,
                               feedback) == Error::None);
        assert(U32(f.tx.data() + f.layout.uart_tx[0] + 4) == 1);
        assert(U16(f.tx.data() + f.layout.uart_tx[0] + 12) == 16);
        auto first_chunk = U32(f.tx.data() + f.layout.uart_tx[0]);
        const auto uart_offset = f.layout.uart_tx[0];
        std::array<uint8_t, 32> uart_snapshot;
        std::copy_n(f.tx.data() + uart_offset, uart_snapshot.size(), uart_snapshot.data());
        for (unsigned refresh = 0; refresh < 3; ++refresh) {
            assert(f.core->Publish({f.tx.data(), f.tx.size()}, native_now,
                                   feedback, false) == Error::None);
            assert(std::equal(uart_snapshot.begin(), uart_snapshot.end(), f.tx.data() + uart_offset));
        }
        assert(f.core->Publish({f.tx.data(), f.tx.size()}, native_now,
                               feedback) == Error::None);
        assert(U32(f.tx.data() + f.layout.uart_tx[0]) != first_chunk);
        // 租约过期撤销在途批次，下一次服务只发送 YAML 本地保护。
        native_now += 20000;
        CaptureIo(*f.core, feedback, native_now);
        assert(feedback.can[2].count == 0);
        ServiceIo(*f.core, native_now);
        assert(f.core->state() == BusinessState::Protected && !uarts[0].busy);
        ServiceIo(*f.core, native_now);
        assert(cans[2].frames[0].data[0] == 0 && cans[2].aborts > 1);
    }
    {
        Fixture f;
        // 四个完整块接纳后队列满，第五块必须等待空间，ACK 不得提前推进。
        for (unsigned i = 1; i <= 5; ++i) {
            f.Header(i, 0);
            f.Uart(i);
            f.Process();
        }
        Core::Feedback feedback;
        assert(f.core->Publish({f.tx.data(), f.tx.size()}, native_now,
                               feedback) == Error::None);
        assert(U32(f.tx.data() + f.layout.uart_tx[0] + 4) == 4);
        ServiceIo(*f.core, native_now);
        f.Header(6, 0);
        f.Uart(5);
        f.Process();
        assert(f.core->Publish({f.tx.data(), f.tx.size()}, native_now,
                               feedback) == Error::None);
        assert(U32(f.tx.data() + f.layout.uart_tx[0] + 4) == 5);
        Actions status{};
        status.status_sequence = 7;
        status.status_target = 2;
        status.status_channel = 1;
        DispatchIo(*f.core, status, native_now);
        assert(f.core->Publish({f.tx.data(), f.tx.size()}, native_now,
                               feedback) == Error::None);
        assert(U32(f.tx.data() + f.layout.diagnostics + 208) == 0);
    }
    {
        Fixture f;
        f.Header(1);
        f.Can(1);
        f.Process();
        unsigned before = cans[2].batches;
        delay_admission = true;
        ServiceIo(*f.core, native_now);
        delay_admission = false;
        assert(f.core->state() == BusinessState::Protected &&
               cans[2].batches == before);
    }
    std::puts(
        "EtherCAT real IO backend contracts passed (mock peripheral "
        "boundaries).");
}

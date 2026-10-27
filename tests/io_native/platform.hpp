/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <cassert>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>
struct device {
    unsigned index;
};
const device* NativeDevice(const char*);
#define DT_ALIAS(name) #name
#define DEVICE_DT_GET(name) NativeDevice(name)
inline bool device_is_ready(const device*) {
    return true;
}
using k_timeout_t = int;
constexpr int K_NO_WAIT = 0;
extern uint64_t native_now;
inline int64_t k_uptime_ticks() {
    return native_now;
}
inline uint64_t k_ticks_to_us_floor64(uint64_t value) {
    return value;
}
struct k_msgq {
    char* data;
    size_t size, depth, head, count;
};
inline void k_msgq_init(k_msgq* q, char* data, size_t size, size_t depth) {
    *q = {data, size, depth, 0, 0};
}
inline int k_msgq_put(k_msgq* q, const void* data, int) {
    if (q->count == q->depth) {
        return -ENOSPC;
    }
    std::memcpy(q->data + ((q->head + q->count++) % q->depth) * q->size, data,
                q->size);
    return 0;
}
inline int k_msgq_get(k_msgq* q, void* data, int) {
    if (!q->count) {
        return -ENOMSG;
    }
    std::memcpy(data, q->data + q->head * q->size, q->size);
    q->head = (q->head + 1) % q->depth;
    --q->count;
    return 0;
}
inline void k_msgq_purge(k_msgq* q) {
    q->head = q->count = 0;
}
using atomic_t = int32_t;
inline int atomic_get(const atomic_t* p) {
    return *p;
}
inline int atomic_set(atomic_t* p, int v) {
    int old = *p;
    *p = v;
    return old;
}
inline void atomic_clear(atomic_t* p) {
    *p = 0;
}
inline void atomic_inc(atomic_t* p) {
    ++*p;
}
inline void atomic_add(atomic_t* p, int v) {
    *p += v;
}
inline bool atomic_cas(atomic_t* p, int a, int b) {
    if (*p != a) {
        return false;
    }
    *p = b;
    return true;
}
#define __nocache
#define __aligned(n) alignas(n)
struct can_frame {
    uint32_t id{};
    uint8_t dlc{}, flags{}, data[8]{};
};
struct can_filter {
    uint32_t id{}, mask{};
};
enum can_state { CAN_STATE_ERROR_ACTIVE, CAN_STATE_BUS_OFF };
struct can_bus_err_cnt {
    uint8_t tx_err_cnt{}, rx_err_cnt{};
};
constexpr int CAN_MODE_NORMAL = 0;
using can_tx_callback_t = void (*)(const device*, int, void*);
using can_rx_callback_t = void (*)(const device*, can_frame*, void*);
int can_set_mode(const device*, int);
int can_set_bitrate(const device*, uint32_t);
int can_add_rx_filter(const device*, can_rx_callback_t, void*,
                      const can_filter*);
int can_start(const device*);
int can_stop(const device*);
int can_get_state(const device*, can_state*, can_bus_err_cnt*);
enum {
    UART_TX_DONE,
    UART_TX_ABORTED,
    UART_RX_RDY,
    UART_RX_BUF_REQUEST,
    UART_RX_STOPPED,
    UART_RX_DISABLED
};
struct uart_event {
    int type;
    struct {
        struct {
            uint8_t* buf;
            size_t offset, len;
        } rx;
    } data;
};
using uart_callback_t = void (*)(const device*, uart_event*, void*);
struct uart_config {
    int parity{}, stop_bits{};
};
constexpr int UART_CFG_PARITY_EVEN = 1, UART_CFG_STOP_BITS_2 = 2;
int uart_callback_set(const device*, uart_callback_t, void*);
int uart_rx_enable(const device*, uint8_t*, size_t, int32_t);
int uart_rx_disable(const device*);
int uart_rx_buf_rsp(const device*, uint8_t*, size_t);
int uart_config_get(const device*, uart_config*);
int uart_configure(const device*, const uart_config*);
int uart_tx(const device*, const uint8_t*, size_t, int32_t);
int uart_tx_abort(const device*);
int uart_irq_tx_complete(const device*);

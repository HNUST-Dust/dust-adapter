/*
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file usb_transport.cpp
 * @brief 管理 USB 通信会话及其收发状态。
 * @details 实现封装 Zephyr 设备
 * API、硬件初始化和异步回调。共享状态在中断、回调与线程之间访问时使用原子量、内核队列或短临界区保护。
 */

#include "drivers/usb_transport.hpp"

#include <errno.h>
#include <math.h>
#include <string.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

#include "drivers/usb_identity.hpp"
#include "usbd_core.h"

LOG_MODULE_REGISTER(usb_transport, LOG_LEVEL_INF);

namespace {

#if defined(CONFIG_CHERRYUSB) && CONFIG_CHERRYUSB && \
    defined(CONFIG_CHERRYUSB_DEVICE) && CONFIG_CHERRYUSB_DEVICE

/**
 * @brief 在回调与消费者之间保存的原始 USB OUT 传输。
 */
struct UsbReceiveFrame {
    uint32_t receive_timestamp_us;            ///< 回调单调时间的低 32 位。
    uint16_t len;                             ///< data 中的有效字节数。
    uint8_t data[DUST_USB_OUT_BUFFER_BYTES];  ///< DMA 接收缓冲区的副本。
};

// 回调不阻塞；队列已满时增加接收丢失计数。
K_MSGQ_DEFINE(usb_receive_frames, sizeof(UsbReceiveFrame), 16, 4);

constexpr int kUsbRxPriority = 7;
constexpr uint8_t kBusId = 0U;

#if DT_NODE_EXISTS(DT_ALIAS(dust_usb))
constexpr uintptr_t kUsbRegBase = DT_REG_ADDR(DT_ALIAS(dust_usb));
constexpr bool kHasUsbNode = true;
#else
constexpr uintptr_t kUsbRegBase = 0U;
constexpr bool kHasUsbNode = false;
#endif

constexpr uint8_t kVendorInEp = 0x81U;
constexpr uint8_t kVendorOutEp = 0x01U;
constexpr uint8_t kVendorInterface = 0x00U;
constexpr uint16_t kVendorPacketSize = 64U;
// 正式版 High-Speed Bulk 传输；传输大小与上位机 ABI 一致。
constexpr uint8_t kVendorEndpointType = USB_ENDPOINT_TYPE_BULK;
constexpr uint16_t kVendorHsMaxPacketSize = 512U;
constexpr uint8_t kVendorIntervalFs = 0U;
constexpr uint8_t kVendorIntervalHs = 0U;

#define WBR_CONTROL_VENDOR_INTERRUPT_DESCRIPTOR_LEN (9 + 7 + 7)
#define WBR_CONTROL_USB_CONFIG_SIZE \
    (9 + WBR_CONTROL_VENDOR_INTERRUPT_DESCRIPTOR_LEN)

USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX uint8_t
    g_vendor_read_buffer[DUST_USB_OUT_BUFFER_BYTES];
USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX uint8_t g_vendor_tx_buffer[160];

volatile bool g_started = false;
volatile bool g_configured = false;
volatile bool g_vendor_tx_busy = false;
volatile bool g_vendor_out_armed = false;

K_SEM_DEFINE(g_usb_tx_complete, 0, 1);
atomic_t g_usb_connection_epoch;
atomic_t g_usb_received_packets;
atomic_t g_usb_submitted_packets;
atomic_t g_usb_completed_packets;
atomic_t g_usb_receive_queue_drops;
atomic_t g_usb_endpoint_errors;
atomic_t g_usb_last_receive_us;
atomic_t g_usb_last_complete_us;

// 未选择应用协议时，提供与 ReceiveUsb/SendUsb 相同的原始传输 API。
// ISR 回收 qTD 后，在线程上下文中重新挂接 OUT 接收；
// 应用不得拥有描述符、回调或 DMA
// 缓冲区。
K_SEM_DEFINE(g_raw_rx_kick, 0, 1);
K_THREAD_STACK_DEFINE(g_raw_rx_stack, 1024);
k_thread g_raw_rx_thread;

static uint8_t g_device_descriptor[] = {USB_DEVICE_DESCRIPTOR_INIT(
    USB_2_0, 0x00, 0x00, 0x00, USBD_VID, USBD_PID, 0x0200, 0x01)};

static const uint8_t g_config_descriptor_hs[] = {
    USB_CONFIG_DESCRIPTOR_INIT(WBR_CONTROL_USB_CONFIG_SIZE, 0x01, 0x01,
                               USB_CONFIG_BUS_POWERED, USBD_MAX_POWER),
    USB_INTERFACE_DESCRIPTOR_INIT(kVendorInterface, 0x00, 0x02,
                                  USB_DEVICE_CLASS_VEND_SPECIFIC, 0x00, 0x00,
                                  0x00),
    USB_ENDPOINT_DESCRIPTOR_INIT(kVendorOutEp, kVendorEndpointType,
                                 kVendorHsMaxPacketSize, kVendorIntervalHs),
    USB_ENDPOINT_DESCRIPTOR_INIT(kVendorInEp, kVendorEndpointType,
                                 kVendorHsMaxPacketSize, kVendorIntervalHs),
};

static const uint8_t g_config_descriptor_fs[] = {
    USB_CONFIG_DESCRIPTOR_INIT(WBR_CONTROL_USB_CONFIG_SIZE, 0x01, 0x01,
                               USB_CONFIG_BUS_POWERED, USBD_MAX_POWER),
    USB_INTERFACE_DESCRIPTOR_INIT(kVendorInterface, 0x00, 0x02,
                                  USB_DEVICE_CLASS_VEND_SPECIFIC, 0x00, 0x00,
                                  0x00),
    USB_ENDPOINT_DESCRIPTOR_INIT(kVendorOutEp, kVendorEndpointType,
                                 kVendorPacketSize, kVendorIntervalFs),
    USB_ENDPOINT_DESCRIPTOR_INIT(kVendorInEp, kVendorEndpointType,
                                 kVendorPacketSize, kVendorIntervalFs),
};

static const uint8_t g_device_quality_descriptor[] = {
    USB_DEVICE_QUALIFIER_DESCRIPTOR_INIT(USB_2_0, 0x00, 0x00, 0x00, 0x01),
};

static const uint8_t g_other_speed_config_descriptor_hs[] = {
    USB_OTHER_SPEED_CONFIG_DESCRIPTOR_INIT(WBR_CONTROL_USB_CONFIG_SIZE, 0x01,
                                           0x01, USB_CONFIG_BUS_POWERED,
                                           USBD_MAX_POWER),
    USB_INTERFACE_DESCRIPTOR_INIT(kVendorInterface, 0x00, 0x02,
                                  USB_DEVICE_CLASS_VEND_SPECIFIC, 0x00, 0x00,
                                  0x00),
    USB_ENDPOINT_DESCRIPTOR_INIT(kVendorOutEp, kVendorEndpointType,
                                 kVendorPacketSize, kVendorIntervalFs),
    USB_ENDPOINT_DESCRIPTOR_INIT(kVendorInEp, kVendorEndpointType,
                                 kVendorPacketSize, kVendorIntervalFs),
};

static const uint8_t g_other_speed_config_descriptor_fs[] = {
    USB_OTHER_SPEED_CONFIG_DESCRIPTOR_INIT(WBR_CONTROL_USB_CONFIG_SIZE, 0x01,
                                           0x01, USB_CONFIG_BUS_POWERED,
                                           USBD_MAX_POWER),
    USB_INTERFACE_DESCRIPTOR_INIT(kVendorInterface, 0x00, 0x02,
                                  USB_DEVICE_CLASS_VEND_SPECIFIC, 0x00, 0x00,
                                  0x00),
    USB_ENDPOINT_DESCRIPTOR_INIT(kVendorOutEp, kVendorEndpointType,
                                 kVendorHsMaxPacketSize, kVendorIntervalHs),
    USB_ENDPOINT_DESCRIPTOR_INIT(kVendorInEp, kVendorEndpointType,
                                 kVendorHsMaxPacketSize, kVendorIntervalHs),
};

static char g_serial_number[65]{};

static const char* g_string_descriptors[] = {
    (const char[]){0x09, 0x04},
    "Dust",
    CONFIG_BOARD,
    g_serial_number,
};

static const uint8_t* DeviceDescriptorCallback(uint8_t speed) {
    ARG_UNUSED(speed);
    return g_device_descriptor;
}

static const uint8_t* ConfigDescriptorCallback(uint8_t speed) {
    if (speed == USB_SPEED_HIGH) {
        return g_config_descriptor_hs;
    }

    if (speed == USB_SPEED_FULL) {
        return g_config_descriptor_fs;
    }

    return nullptr;
}

static const uint8_t* DeviceQualityDescriptorCallback(uint8_t speed) {
    ARG_UNUSED(speed);
    return g_device_quality_descriptor;
}

static const uint8_t* OtherSpeedDescriptorCallback(uint8_t speed) {
    if (speed == USB_SPEED_HIGH) {
        return g_other_speed_config_descriptor_hs;
    }

    if (speed == USB_SPEED_FULL) {
        return g_other_speed_config_descriptor_fs;
    }

    return nullptr;
}

static const char* StringDescriptorCallback(uint8_t speed, uint8_t index) {
    ARG_UNUSED(speed);

    if (index >= ARRAY_SIZE(g_string_descriptors)) {
        return nullptr;
    }

    return g_string_descriptors[index];
}

const struct usb_descriptor g_usb_descriptor = {
    .device_descriptor_callback = DeviceDescriptorCallback,
    .config_descriptor_callback = ConfigDescriptorCallback,
    .device_quality_descriptor_callback = DeviceQualityDescriptorCallback,
    .other_speed_descriptor_callback = OtherSpeedDescriptorCallback,
    .string_descriptor_callback = StringDescriptorCallback,
};

/**
 * @brief 端点可用时挂接 OUT 接收；返回零或负端点 errno。
 */
int ArmVendorOut() {
    if (!g_configured || g_vendor_out_armed) {
        return 0;
    }

    // 挂接传输前先发布所有权：快速主机可能在 start_read 返回前完成传输。
    // 回调必须能够清除此标志。
    g_vendor_out_armed = true;
    const int rc =
        usbd_ep_start_read(kBusId, kVendorOutEp, g_vendor_read_buffer,
                           sizeof(g_vendor_read_buffer));
    if (rc < 0) {
        g_vendor_out_armed = false;
        atomic_inc(&g_usb_endpoint_errors);
        return rc;
    }
    return 0;
}

/**
 * @brief 在 USB 生命周期事件中更新端点与会话状态，并清除旧接收数据。
 */
void UsbEventHandler(uint8_t busid, uint8_t event) {
    ARG_UNUSED(busid);

    switch (event) {
        case USBD_EVENT_CONFIGURED:
            g_configured = true;
            g_vendor_tx_busy = false;
            g_vendor_out_armed = false;
            k_sem_give(&g_raw_rx_kick);
            (void)ArmVendorOut();
            break;
        case USBD_EVENT_RESET:
            atomic_inc(&g_usb_connection_epoch);
            k_msgq_purge(&usb_receive_frames);
            g_configured = false;
            g_vendor_tx_busy = false;
            g_vendor_out_armed = false;
            k_sem_give(&g_raw_rx_kick);
            break;
        case USBD_EVENT_DISCONNECTED:
            atomic_inc(&g_usb_connection_epoch);
            k_msgq_purge(&usb_receive_frames);
            g_configured = false;
            g_vendor_tx_busy = false;
            g_vendor_out_armed = false;
            k_sem_give(&g_raw_rx_kick);
            break;
        default:
            break;
    }
}

/**
 * @brief 将有界 DMA 接收块复制到非阻塞队列，并统计溢出。
 */
void EnqueueUsbRx(const uint8_t* data, size_t len) {
    const size_t copy_len = MIN(len, sizeof(UsbReceiveFrame::data));
    if ((data == nullptr) || (copy_len == 0U)) {
        return;
    }

    UsbReceiveFrame frame = {};
    frame.receive_timestamp_us =
        static_cast<uint32_t>(atomic_get(&g_usb_last_receive_us));
    frame.len = static_cast<uint16_t>(copy_len);
    memcpy(frame.data, data, copy_len);
    if (k_msgq_put(&usb_receive_frames, &frame, K_NO_WAIT) != 0) {
        atomic_inc(&g_usb_receive_queue_drops);
    }
}

/**
 * @brief 记录 OUT 回调时间、接收数据入队，并唤醒延后执行的端点接收挂接。
 */
void UsbBulkOutCallback(uint8_t busid, uint8_t ep, uint32_t nbytes) {
    ARG_UNUSED(busid);

    g_vendor_out_armed = false;
    atomic_inc(&g_usb_received_packets);
    atomic_set(&g_usb_last_receive_us, k_cyc_to_us_floor64(k_cycle_get_64()));
    EnqueueUsbRx(g_vendor_read_buffer, MIN(static_cast<size_t>(nbytes),
                                           sizeof(g_vendor_read_buffer)));
    k_sem_give(&g_raw_rx_kick);

    /* 不要在完成回调中复用 HPM qTD。HPM 端口调用此回调时，USBD_IRQHandler()
     * 尚未完成遍历和回收已完成的 qTD。此时重新挂接同一端点可能返回成功，但新
     * qTD 实际未激活。原始接收工作线程观察到 g_vendor_out_armed == false
     * 后，会在 USB ISR 返回后提交下一次接收。 */
    ARG_UNUSED(ep);
}

/**
 * @brief 记录 IN 完成状态，解除唯一发送者的完成等待。
 */
void UsbBulkInCallback(uint8_t busid, uint8_t ep, uint32_t nbytes) {
    ARG_UNUSED(busid);
    ARG_UNUSED(ep);
    ARG_UNUSED(nbytes);
    g_vendor_tx_busy = false;
    atomic_inc(&g_usb_completed_packets);
    atomic_set(&g_usb_last_complete_us, k_cyc_to_us_floor64(k_cycle_get_64()));
    k_sem_give(&g_usb_tx_complete);
}

struct usbd_endpoint g_vendor_out_ep = {
    .ep_addr = kVendorOutEp,
    .ep_cb = UsbBulkOutCallback,
};

struct usbd_endpoint g_vendor_in_ep = {
    .ep_addr = kVendorInEp,
    .ep_cb = UsbBulkInCallback,
};

static struct usbd_interface g_vendor_intf;

/**
 * @brief 回调结束并回收 DMA 描述符后，在线程上下文重新挂接 OUT 接收。
 */
void UsbRxLoop(void*, void*, void*) {
    for (;;) {
        (void)k_sem_take(&g_raw_rx_kick, K_FOREVER);
        if (ArmVendorOut() < 0) {
            // 端点提交暂时失败时等待重试，避免忙循环。
            k_sleep(K_MSEC(1));
            k_sem_give(&g_raw_rx_kick);
        }
    }
}

#endif

}  // namespace

namespace platform {

/**
 * @brief 初始化 USB 端点及接收线程。
 * @return 成功返回零，否则返回负 errno。
 */
int InitializeUsbTransport() {
#if defined(CONFIG_CHERRYUSB) && CONFIG_CHERRYUSB && \
    defined(CONFIG_CHERRYUSB_DEVICE) && CONFIG_CHERRYUSB_DEVICE
    if (g_started) {
        return 0;
    }

    if (!kHasUsbNode) {
        return -ENODEV;
    }

    uint16_t pid = 0;
    const int identity_result =
        ReadUsbIdentity(pid, g_serial_number, sizeof(g_serial_number));
    if (identity_result) {
        return identity_result;
    }
    // USB 设备描述符的 idProduct 固定位于字节 10..11，采用小端编码。
    g_device_descriptor[10] = uint8_t(pid);
    g_device_descriptor[11] = uint8_t(pid >> 8);
    usbd_desc_register(kBusId, &g_usb_descriptor);
    usbd_add_interface(kBusId, &g_vendor_intf);
    usbd_add_endpoint(kBusId, &g_vendor_out_ep);
    usbd_add_endpoint(kBusId, &g_vendor_in_ep);

    const int rc = usbd_initialize(kBusId, kUsbRegBase, UsbEventHandler);
    if (rc < 0) {
        return rc;
    }

    g_started = true;

    k_thread_create(&g_raw_rx_thread, g_raw_rx_stack,
                    K_THREAD_STACK_SIZEOF(g_raw_rx_stack), UsbRxLoop, nullptr,
                    nullptr, nullptr, kUsbRxPriority, 0, K_NO_WAIT);
    (void)k_thread_name_set(&g_raw_rx_thread, "usb_rx");

    return 0;
#else
    return -ENOTSUP;
#endif
}

/**
 * @brief 将当前原子传输计数器复制到 stats；空指针被忽略。
 */
void ReadUsbTransportStats(UsbTransportStats* stats) {
    if (stats == nullptr) {
        return;
    }
    *stats = {};
#if defined(CONFIG_CHERRYUSB) && defined(CONFIG_CHERRYUSB_DEVICE)
    stats->received_packets = atomic_get(&g_usb_received_packets);
    stats->submitted_packets = atomic_get(&g_usb_submitted_packets);
    stats->completed_packets = atomic_get(&g_usb_completed_packets);
    stats->receive_queue_drops = atomic_get(&g_usb_receive_queue_drops);
    stats->endpoint_errors = atomic_get(&g_usb_endpoint_errors);
    stats->last_receive_us = atomic_get(&g_usb_last_receive_us);
    stats->last_complete_us = atomic_get(&g_usb_last_complete_us);
#endif
}

/**
 * @brief 返回连接代次，用于拒绝旧 USB 连接的数据。
 */
uint32_t UsbConnectionEpoch() {
    return uint32_t(atomic_get(&g_usb_connection_epoch));
}

/**
 * @brief 返回 USB 是否已启动且已被主机配置。
 */
bool IsUsbConfigured() {
#if defined(CONFIG_CHERRYUSB) && CONFIG_CHERRYUSB && \
    defined(CONFIG_CHERRYUSB_DEVICE) && CONFIG_CHERRYUSB_DEVICE
    return g_started && g_configured;
#else
    return false;
#endif
}

/**
 * @brief 由唯一 IN 发送者提交数据；返回前完成数据复制。
 * @return 返回已提交字节数或负 errno；提交不代表传输完成。
 */
int SendUsb(const uint8_t* data, size_t len) {
#if defined(CONFIG_CHERRYUSB) && CONFIG_CHERRYUSB && \
    defined(CONFIG_CHERRYUSB_DEVICE) && CONFIG_CHERRYUSB_DEVICE
    if ((data == nullptr) || (len == 0U)) {
        return -EINVAL;
    }

    if (!IsUsbConfigured()) {
        return -EAGAIN;
    }

    if (len > sizeof(g_vendor_tx_buffer)) {
        return -EMSGSIZE;
    }

    if (g_vendor_tx_busy) {
        return -EBUSY;
    }

    k_sem_reset(&g_usb_tx_complete);
    memcpy(g_vendor_tx_buffer, data, len);
    g_vendor_tx_busy = true;

    const int rc = usbd_ep_start_write(kBusId, kVendorInEp, g_vendor_tx_buffer,
                                       static_cast<uint32_t>(len));
    if (rc < 0) {
        g_vendor_tx_busy = false;
        atomic_inc(&g_usb_endpoint_errors);
        return rc;
    }
    atomic_inc(&g_usb_submitted_packets);

    return static_cast<int>(len);
#else
    ARG_UNUSED(data);
    ARG_UNUSED(len);
    return -ENOTSUP;
#endif
}

/**
 * @brief 由唯一发送者等待最近提交的 IN 传输。
 * @return 完成返回零，否则返回负 errno；timeout_ms 单位为毫秒。
 */
int WaitUsbTxComplete(int32_t timeout_ms) {
#if defined(CONFIG_CHERRYUSB) && defined(CONFIG_CHERRYUSB_DEVICE)
    const k_timeout_t timeout = timeout_ms < 0 ? K_FOREVER : K_MSEC(timeout_ms);
    return k_sem_take(&g_usb_tx_complete, timeout);
#else
    ARG_UNUSED(timeout_ms);
    return -ENOTSUP;
#endif
}

/**
 * @brief 在 timeout_ms 内将下一条排队的 OUT 帧复制给唯一消费者。
 * @return 成功返回零，否则返回负 errno；容量不足时仍消费该帧并截断复制。
 */
int ReceiveUsb(uint8_t* out, size_t capacity, size_t* out_len,
               int32_t timeout_ms, uint32_t* receive_timestamp_us) {
    if ((out == nullptr) || (out_len == nullptr) || (capacity == 0U)) {
        return -EINVAL;
    }

    *out_len = 0U;
    if (receive_timestamp_us != nullptr) {
        *receive_timestamp_us = 0;
    }

#if defined(CONFIG_CHERRYUSB) && CONFIG_CHERRYUSB && \
    defined(CONFIG_CHERRYUSB_DEVICE) && CONFIG_CHERRYUSB_DEVICE
    UsbReceiveFrame chunk = {};
    const k_timeout_t timeout =
        timeout_ms < 0 ? K_FOREVER
                       : (timeout_ms > 0 ? K_MSEC(timeout_ms) : K_NO_WAIT);
    const int rc = k_msgq_get(&usb_receive_frames, &chunk, timeout);
    if (rc != 0) {
        return rc;
    }

    if (receive_timestamp_us != nullptr) {
        *receive_timestamp_us = chunk.receive_timestamp_us;
    }
    const size_t copy_len = MIN(capacity, static_cast<size_t>(chunk.len));
    memcpy(out, chunk.data, copy_len);
    *out_len = copy_len;
    return (copy_len == static_cast<size_t>(chunk.len)) ? 0 : -EMSGSIZE;
#else
    ARG_UNUSED(timeout_ms);
    return -ENOTSUP;
#endif
}

}  // namespace platform

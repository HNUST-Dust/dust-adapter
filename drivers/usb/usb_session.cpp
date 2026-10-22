/*
 * SPDX-License-Identifier: Apache-2.0
 */

/**
* @file drivers/usb/usb_session.cpp
 * @ingroup wbr_platform
 * @brief 管理 USB 通信会话及其收发状态。
 * @details 实现封装 Zephyr 设备 API、硬件初始化和异步回调。共享状态在中断、回调与线程之间访问时使用原子量、内核队列或短临界区保护。
 */

#include <errno.h>
#include <math.h>
#include <string.h>

#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>
#include <zephyr/sys/atomic.h>

#include <msg/usb_raw_frame_queue.hpp>
#include <dust_adapter/usb_session.hpp>
#include <dust_adapter/thread_priorities.hpp>


#include "usbd_core.h"

LOG_MODULE_REGISTER(usb_session, LOG_LEVEL_INF);

namespace {

#if defined(CONFIG_CHERRYUSB) && CONFIG_CHERRYUSB && defined(CONFIG_CHERRYUSB_DEVICE) && \
	CONFIG_CHERRYUSB_DEVICE

constexpr uint8_t kBusId = 0U;

#if DT_NODE_EXISTS(DT_NODELABEL(cherryusb_usb0))
constexpr uintptr_t kUsbRegBase = DT_REG_ADDR(DT_NODELABEL(cherryusb_usb0));
constexpr bool kHasUsbNode = true;
#else
constexpr uintptr_t kUsbRegBase = 0U;
constexpr bool kHasUsbNode = false;
#endif

constexpr uint8_t kVendorInEp = 0x81U;
constexpr uint8_t kVendorOutEp = 0x01U;
constexpr uint8_t kVendorInterface = 0x00U;
constexpr uint16_t kVendorPacketSize = 64U;
// Production High-Speed Bulk transport; transfer sizes match the host ABI.
constexpr uint8_t kVendorEndpointType = USB_ENDPOINT_TYPE_BULK;
constexpr uint16_t kVendorHsMaxPacketSize = 512U;
constexpr uint8_t kVendorIntervalFs = 0U;
constexpr uint8_t kVendorIntervalHs = 0U;

#define WBR_CONTROL_VENDOR_INTERRUPT_DESCRIPTOR_LEN (9 + 7 + 7)
#define WBR_CONTROL_USB_CONFIG_SIZE (9 + WBR_CONTROL_VENDOR_INTERRUPT_DESCRIPTOR_LEN)

USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX
uint8_t g_vendor_read_buffer[
    WBR_USB_RAW_OUT_BYTES
];
USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX uint8_t g_vendor_tx_buffer[kVendorPacketSize];

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

// With no application protocol selected, provide the same raw transport API
// used by ReceiveUsb/SendUsb. Rearm OUT in thread context, after the ISR retires
// its qTD; applications must not own descriptors, callbacks, or DMA buffers.
#define WBR_USB_RAW_TRANSPORT 1
K_SEM_DEFINE(g_raw_rx_kick, 0, 1);
K_THREAD_STACK_DEFINE(g_raw_rx_stack, 1024);
k_thread g_raw_rx_thread;



static const uint8_t g_device_descriptor[] = {
	USB_DEVICE_DESCRIPTOR_INIT(USB_2_0, 0x00, 0x00, 0x00, USBD_VID, USBD_PID, 0x0200, 0x01)
};

static const uint8_t g_config_descriptor_hs[] = {
	USB_CONFIG_DESCRIPTOR_INIT(
		WBR_CONTROL_USB_CONFIG_SIZE,
		0x01,
		0x01,
		USB_CONFIG_BUS_POWERED,
		USBD_MAX_POWER),
	USB_INTERFACE_DESCRIPTOR_INIT(kVendorInterface, 0x00, 0x02,
		USB_DEVICE_CLASS_VEND_SPECIFIC, 0x00, 0x00, 0x00),
	USB_ENDPOINT_DESCRIPTOR_INIT(kVendorOutEp, kVendorEndpointType,
		kVendorHsMaxPacketSize, kVendorIntervalHs),
	USB_ENDPOINT_DESCRIPTOR_INIT(kVendorInEp, kVendorEndpointType,
		kVendorHsMaxPacketSize, kVendorIntervalHs),
};

static const uint8_t g_config_descriptor_fs[] = {
	USB_CONFIG_DESCRIPTOR_INIT(
		WBR_CONTROL_USB_CONFIG_SIZE,
		0x01,
		0x01,
		USB_CONFIG_BUS_POWERED,
		USBD_MAX_POWER),
	USB_INTERFACE_DESCRIPTOR_INIT(kVendorInterface, 0x00, 0x02,
		USB_DEVICE_CLASS_VEND_SPECIFIC, 0x00, 0x00, 0x00),
	USB_ENDPOINT_DESCRIPTOR_INIT(kVendorOutEp, kVendorEndpointType,
		kVendorPacketSize, kVendorIntervalFs),
	USB_ENDPOINT_DESCRIPTOR_INIT(kVendorInEp, kVendorEndpointType,
		kVendorPacketSize, kVendorIntervalFs),
};

static const uint8_t g_device_quality_descriptor[] = {
	USB_DEVICE_QUALIFIER_DESCRIPTOR_INIT(USB_2_0, 0x00, 0x00, 0x00, 0x01),
};

static const uint8_t g_other_speed_config_descriptor_hs[] = {
	USB_OTHER_SPEED_CONFIG_DESCRIPTOR_INIT(
		WBR_CONTROL_USB_CONFIG_SIZE,
		0x01,
		0x01,
		USB_CONFIG_BUS_POWERED,
		USBD_MAX_POWER),
	USB_INTERFACE_DESCRIPTOR_INIT(kVendorInterface, 0x00, 0x02,
		USB_DEVICE_CLASS_VEND_SPECIFIC, 0x00, 0x00, 0x00),
	USB_ENDPOINT_DESCRIPTOR_INIT(kVendorOutEp, kVendorEndpointType,
		kVendorPacketSize, kVendorIntervalFs),
	USB_ENDPOINT_DESCRIPTOR_INIT(kVendorInEp, kVendorEndpointType,
		kVendorPacketSize, kVendorIntervalFs),
};

static const uint8_t g_other_speed_config_descriptor_fs[] = {
	USB_OTHER_SPEED_CONFIG_DESCRIPTOR_INIT(
		WBR_CONTROL_USB_CONFIG_SIZE,
		0x01,
		0x01,
		USB_CONFIG_BUS_POWERED,
		USBD_MAX_POWER),
	USB_INTERFACE_DESCRIPTOR_INIT(kVendorInterface, 0x00, 0x02,
		USB_DEVICE_CLASS_VEND_SPECIFIC, 0x00, 0x00, 0x00),
	USB_ENDPOINT_DESCRIPTOR_INIT(kVendorOutEp, kVendorEndpointType,
		kVendorHsMaxPacketSize, kVendorIntervalHs),
	USB_ENDPOINT_DESCRIPTOR_INIT(kVendorInEp, kVendorEndpointType,
		kVendorHsMaxPacketSize, kVendorIntervalHs),
};

static const char *g_string_descriptors[] = {
	(const char[]){0x09, 0x04},
	"HPMicro",
	"Dust USB Adapter",
	"0001",
};

static const uint8_t *DeviceDescriptorCallback(uint8_t speed)
{
	ARG_UNUSED(speed);
	return g_device_descriptor;
}

static const uint8_t *ConfigDescriptorCallback(uint8_t speed)
{
	if (speed == USB_SPEED_HIGH) {
		return g_config_descriptor_hs;
	}

	if (speed == USB_SPEED_FULL) {
		return g_config_descriptor_fs;
	}

	return nullptr;
}

static const uint8_t *DeviceQualityDescriptorCallback(uint8_t speed)
{
	ARG_UNUSED(speed);
	return g_device_quality_descriptor;
}

static const uint8_t *OtherSpeedDescriptorCallback(uint8_t speed)
{
	if (speed == USB_SPEED_HIGH) {
		return g_other_speed_config_descriptor_hs;
	}

	if (speed == USB_SPEED_FULL) {
		return g_other_speed_config_descriptor_fs;
	}

	return nullptr;
}

static const char *StringDescriptorCallback(uint8_t speed, uint8_t index)
{
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

int ArmVendorOut()
{
	if (!g_configured || g_vendor_out_armed) {
		return 0;
	}

	// Publish ownership before priming: a fast host can complete the transfer
	// before start_read returns. The callback must be able to clear this flag.
	g_vendor_out_armed = true;
	const int rc = usbd_ep_start_read(
		kBusId, kVendorOutEp, g_vendor_read_buffer, sizeof(g_vendor_read_buffer));
	if (rc < 0) {
		g_vendor_out_armed = false;
		atomic_inc(&g_usb_endpoint_errors);
		return rc;
	}
	return 0;
}

void UsbEventHandler(uint8_t busid, uint8_t event)
{
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
		msg::usb_raw_frames.Clear();
		g_configured = false;
		g_vendor_tx_busy = false;
		g_vendor_out_armed = false;
		k_sem_give(&g_raw_rx_kick);
		break;
	case USBD_EVENT_DISCONNECTED:
		atomic_inc(&g_usb_connection_epoch);
		msg::usb_raw_frames.Clear();
		g_configured = false;
		g_vendor_tx_busy = false;
		g_vendor_out_armed = false;
		k_sem_give(&g_raw_rx_kick);
		break;
	default:
		break;
	}
}

void EnqueueUsbRx(const uint8_t *data, size_t len)
{
	const size_t copy_len = MIN(len, msg::kUsbRawChunkSize);
	if ((data == nullptr) || (copy_len == 0U)) {
		return;
	}

	msg::UsbRawFrameMessage frame = {};
	frame.receive_timestamp_us = static_cast<uint32_t>(atomic_get(&g_usb_last_receive_us));
	frame.len = static_cast<uint16_t>(copy_len);
	memcpy(frame.data, data, copy_len);
	if (msg::usb_raw_frames.TryPush(frame) != 0) {
		atomic_inc(&g_usb_receive_queue_drops);
	}
}

void UsbBulkOutCallback(uint8_t busid, uint8_t ep, uint32_t nbytes)
{
	ARG_UNUSED(busid);

	g_vendor_out_armed = false;
	atomic_inc(&g_usb_received_packets);
	atomic_set(&g_usb_last_receive_us, k_cyc_to_us_floor64(k_cycle_get_64()));
	EnqueueUsbRx(g_vendor_read_buffer, MIN(static_cast<size_t>(nbytes),
					       sizeof(g_vendor_read_buffer)));
	k_sem_give(&g_raw_rx_kick);

	/*
	 * Do not reuse the HPM qTD from inside its completion callback.  The HPM
	 * port invokes this callback before USBD_IRQHandler() has finished walking
	 * and retiring the completed qTD.  Re-priming the same endpoint here can
	 * therefore be reported as successful while the new qTD never becomes
	 * active.  The raw receive worker observes g_vendor_out_armed == false and submits the
	 * next receive after the USB ISR has returned.
	 */
	ARG_UNUSED(ep);
}

void UsbBulkInCallback(uint8_t busid, uint8_t ep, uint32_t nbytes)
{
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

void RawUsbReceiveLoop(void *, void *, void *)
{
	for (;;) {
		(void)k_sem_take(&g_raw_rx_kick, K_FOREVER);
		if (ArmVendorOut() < 0) {
			// Retry a transient endpoint submission failure without spinning.
			k_sleep(K_MSEC(1));
			k_sem_give(&g_raw_rx_kick);
		}
	}
}



#endif

}  // namespace

namespace platform {

int InitializeUsbSession(bool polling)
{
#if (defined(CONFIG_WBR_CONTROL_PC_LINK) && CONFIG_WBR_CONTROL_PC_LINK) ||                         \
	(defined(CONFIG_WBR_CONTROL_USB_MPC) && CONFIG_WBR_CONTROL_USB_MPC)
	if (polling)
		return -ENOTSUP;
#endif
#if defined(CONFIG_CHERRYUSB) && CONFIG_CHERRYUSB && defined(CONFIG_CHERRYUSB_DEVICE) &&           \
	CONFIG_CHERRYUSB_DEVICE
	if (g_started) {
		return 0;
	}

	if (!kHasUsbNode) {
		return -ENODEV;
	}

	usbd_desc_register(kBusId, &g_usb_descriptor);
	usbd_add_interface(kBusId, &g_vendor_intf);
	usbd_add_endpoint(kBusId, &g_vendor_out_ep);
	usbd_add_endpoint(kBusId, &g_vendor_in_ep);

	const int rc = usbd_initialize(kBusId, kUsbRegBase, UsbEventHandler);
	if (rc < 0) {
		return rc;
	}

	g_started = true;

	if (!polling)
		k_thread_create(&g_raw_rx_thread, g_raw_rx_stack,
				K_THREAD_STACK_SIZEOF(g_raw_rx_stack), RawUsbReceiveLoop, nullptr,
				nullptr, nullptr, dust_adapter::thread_priority::kUsbRx, 0,
				K_NO_WAIT);
	if (!polling)
		(void)k_thread_name_set(&g_raw_rx_thread, "usb_raw_rx");



	return 0;
#else
	return -ENOTSUP;
#endif
}

void ReadUsbSessionStats(UsbSessionStats *stats)
{
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

uint32_t UsbConnectionEpoch() { return uint32_t(atomic_get(&g_usb_connection_epoch)); }

void PollUsbSession()
{
	if (g_started)
		(void)ArmVendorOut();
}

bool IsUsbConfigured()
{
#if defined(CONFIG_CHERRYUSB) && CONFIG_CHERRYUSB && defined(CONFIG_CHERRYUSB_DEVICE) && \
	CONFIG_CHERRYUSB_DEVICE
	return g_started && g_configured;
#else
	return false;
#endif
}

int SendUsb(const uint8_t *data, size_t len)
{
#if defined(CONFIG_CHERRYUSB) && CONFIG_CHERRYUSB && defined(CONFIG_CHERRYUSB_DEVICE) && \
	CONFIG_CHERRYUSB_DEVICE
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

	const int rc = usbd_ep_start_write(
		kBusId, kVendorInEp, g_vendor_tx_buffer, static_cast<uint32_t>(len));
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

int WaitUsbTxComplete(int32_t timeout_ms)
{
#if defined(CONFIG_CHERRYUSB) && defined(CONFIG_CHERRYUSB_DEVICE)
	const k_timeout_t timeout = timeout_ms < 0 ? K_FOREVER : K_MSEC(timeout_ms);
	return k_sem_take(&g_usb_tx_complete, timeout);
#else
	ARG_UNUSED(timeout_ms);
	return -ENOTSUP;
#endif
}

int ReceiveUsb(uint8_t *out, size_t capacity, size_t *out_len, int32_t timeout_ms,
	       uint32_t *receive_timestamp_us)
{
	if ((out == nullptr) || (out_len == nullptr) || (capacity == 0U)) {
		return -EINVAL;
	}

	*out_len = 0U;
	if (receive_timestamp_us != nullptr) {
		*receive_timestamp_us = 0;
	}

#if defined(CONFIG_CHERRYUSB) && CONFIG_CHERRYUSB && defined(CONFIG_CHERRYUSB_DEVICE) && \
	CONFIG_CHERRYUSB_DEVICE
	msg::UsbRawFrameMessage chunk = {};
	const k_timeout_t timeout = timeout_ms < 0 ? K_FOREVER :
		(timeout_ms > 0 ? K_MSEC(timeout_ms) : K_NO_WAIT);
	const int rc = msg::usb_raw_frames.Pop(chunk, timeout);
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

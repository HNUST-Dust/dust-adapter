/* SPDX-License-Identifier: Apache-2.0 */
#include "usb_reports.hpp"

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

#include "drivers/usb_transport.hpp"
namespace {
namespace wire = dust::wire;
using wire::UsbInPacket;
constexpr int kUsbTxPriority = 7;
/**
 * @brief CAN 接收帧及其采集时的会话标识。
 */
struct CanRxForUsb {
    platform::CanReceiveFrame frame;
    uint32_t session;
};
K_MSGQ_DEFINE(can_rx_queue, sizeof(CanRxForUsb), 128, 4);
K_MSGQ_DEFINE(usb_sensor_report_queue, sizeof(UsbInPacket), 128, 4);
K_MSGQ_DEFINE(usb_reply_queue, sizeof(UsbInPacket), 16, 4);
K_SEM_DEFINE(usb_tx_event, 0, 1);
K_THREAD_STACK_DEFINE(usb_tx_stack, 2048);
k_thread usb_tx_thread;
atomic_t published_usb_session_id{};
atomic_t can_drops{};
atomic_t peripheral_drops{};
atomic_t reply_drops{};
atomic_t usb_send_errors{};
uint32_t NowUs() {
    return k_cyc_to_us_floor64(k_cycle_get_64());
}
/**
 * @brief 最多打包六条当前会话的帧，丢弃旧会话帧；有有效帧时返回 true。
 */
bool BuildCanUsbReport(UsbInPacket& packet) {
    const uint32_t session = gateway::CurrentUsbReportSession();
    if (!session) {
        return false;
    }
    CanRxForUsb queued{};
    wire::CanReceiveReport report{};
    report.header.session_id = session;
    while (report.frame_count < wire::kMaxCanFramesPerPacket &&
           !k_msgq_get(&can_rx_queue, &queued, K_NO_WAIT)) {
        if (queued.session != session) {
            continue;
        }
        report.frames[report.frame_count++] = {queued.frame.record,
                                               queued.frame.timestamp_us,
                                               queued.frame.sequence};
    }
    if (!report.frame_count) {
        return false;
    }
    return wire::Encode(report, packet);
}

/**
 * @brief 运行唯一的 USB 发送线程；优先发送回复，并交替处理外设与 CAN 流量。
 */
void UsbTxLoop(void*, void*, void*) {
    bool peripheral_turn = false;
    for (;;) {
        UsbInPacket packet{};
        bool found = !k_msgq_get(&usb_reply_queue, &packet, K_NO_WAIT);
        if (!found && peripheral_turn) {
            found = !k_msgq_get(&usb_sensor_report_queue, &packet, K_NO_WAIT);
        }
        if (!found) {
            found = BuildCanUsbReport(packet);
        }
        if (!found) {
            found = !k_msgq_get(&usb_sensor_report_queue, &packet, K_NO_WAIT);
        }
        if (!found) {
            k_sem_take(&usb_tx_event, K_FOREVER);
            continue;
        }
        peripheral_turn = !peripheral_turn;
        if (!gateway::CurrentUsbReportSession() ||
            wire::U32(packet.data() + 12) !=
                gateway::CurrentUsbReportSession()) {
            continue;
        }
        wire::Put32(packet.data() + 4, NowUs());
        const size_t length = wire::UsbReportLength(packet);
        if (!length ||
            platform::SendUsb(packet.data(), length) != int(length) ||
            platform::WaitUsbTxComplete(20)) {
            atomic_inc(&usb_send_errors);
        }
    }
}

}  // namespace
namespace gateway {
/**
 * @brief 将回复无等待入队并唤醒上行线程；队列溢出计为回复丢失。
 */
void QueueUsbReply(const UsbInPacket& packet) {
    if (k_msgq_put(&usb_reply_queue, &packet, K_NO_WAIT)) {
        atomic_inc(&reply_drops);
    }
    k_sem_give(&usb_tx_event);
}

/**
 * @brief 将 CAN 帧与当前发布会话一起入队；溢出时丢弃，不阻塞。
 */
void QueueCanRxForUsb(const platform::CanReceiveFrame& frame) {
    const uint32_t session = gateway::CurrentUsbReportSession();
    if (!session) {
        return;
    }
    const CanRxForUsb queued{frame, session};
    if (k_msgq_put(&can_rx_queue, &queued, K_NO_WAIT)) {
        atomic_inc(&can_drops);
    } else {
        k_sem_give(&usb_tx_event);
    }
}

/**
 * @brief 返回当前会话标识；零表示未激活。
 */
uint32_t CurrentUsbReportSession() {
    return uint32_t(atomic_get(&published_usb_session_id));
}

/**
 * @brief 将携带会话标识的外设数据包无等待入队；拒绝时返回 false。
 */
bool QueueSensorReportForUsb(const UsbInPacket& packet) {
    if (!CurrentUsbReportSession()) {
        return false;
    }
    if (k_msgq_put(&usb_sensor_report_queue, &packet, K_NO_WAIT)) {
        atomic_inc(&peripheral_drops);
        return false;
    }
    k_sem_give(&usb_tx_event);
    return true;
}

void StartUsbReports() {
    k_thread_create(&usb_tx_thread, usb_tx_stack,
                    K_THREAD_STACK_SIZEOF(usb_tx_stack), UsbTxLoop, nullptr,
                    nullptr, nullptr, kUsbTxPriority, 0, K_NO_WAIT);
}
void ResetUsbReportQueues() {
    k_msgq_purge(&can_rx_queue);
    k_msgq_purge(&usb_sensor_report_queue);
    k_msgq_purge(&usb_reply_queue);
}
void SetUsbReportSession(uint32_t session) {
    atomic_set(&published_usb_session_id, session);
}
UsbReportStats ReadUsbReportStats() {
    return {uint32_t(atomic_get(&can_drops)),
            uint32_t(atomic_get(&peripheral_drops)),
            uint32_t(atomic_get(&reply_drops)),
            uint32_t(atomic_get(&usb_send_errors))};
}
}  // namespace gateway

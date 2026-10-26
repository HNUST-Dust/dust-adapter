/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include "drivers/can_port.hpp"
#include "usb_protocol.hpp"
namespace gateway {
/**
 * @brief 返回当前会话标识；零表示未激活。
 */
uint32_t CurrentUsbReportSession();

/**
 * @brief 将携带会话标识的外设数据包无等待入队；拒绝时返回 false。
 */
bool QueueSensorReportForUsb(const dust::wire::UsbInPacket&);

/** @brief USB 上行排队和发送的累计统计。 */
struct UsbReportStats {
    uint32_t can_rx_drops;
    uint32_t sensor_report_drops;
    uint32_t reply_drops;
    uint32_t send_errors;
};
/** @brief 启动唯一 USB 发送线程，初始化时调用一次。 */
void StartUsbReports();
/** @brief 无等待排队发送控制回复；满时计数并丢弃。 */
void QueueUsbReply(const dust::wire::UsbInPacket& packet);
/** @brief CAN 接收回调：记录采集时会话并入队，供 USB 上报。 */
void QueueCanRxForUsb(const platform::CanReceiveFrame& frame);
/** @brief 清空旧会话队列，由会话所有者在建立新会话时调用。 */
void ResetUsbReportQueues();
/** @brief 设置采集数据使用的发布会话；零表示禁止发布。 */
void SetUsbReportSession(uint32_t session);
UsbReportStats ReadUsbReportStats();
}  // namespace gateway

/* SPDX-License-Identifier: Apache-2.0 */

/**
 * @file can_port.hpp
 * @brief CAN 接纳、接收回调和传输诊断。
 */
#pragma once
#include "usb_protocol.hpp"
namespace platform {

/**
 * @brief 原始 CAN 记录，包含 MCU 接收时间和每总线序号。
 */
struct CanReceiveFrame {
    dust::wire::CanFrame record;
    uint32_t timestamp_us, sequence;
};
using CanReceiveCallback = void (*)(const CanReceiveFrame&);

/**
 * @brief CAN FIFO、控制器及完成计数器快照。
 */
struct CanStats {
    uint32_t received, submitted, completed, errors, dropped;
    uint32_t queue_depth, state, tx_errors, rx_errors;
};

/**
 * @brief 初始化已配置总线、精确 RX 过滤器和 TX 工作线程；返回零或负 errno。
 */
int InitializeCanPorts(CanReceiveCallback callback);

/**
 * @brief 为每个已初始化的逻辑 CAN 通道返回一个可用性位。
 */
uint32_t CanChannelMask();

/**
 * @brief 返回总线编号是否在范围内且已初始化。
 */
bool CanAvailable(uint8_t bus);
// 发送 ID、标志和 RX 过滤器来自 config/can_devices.yaml。
// 发布前检查整包接纳条件；采用 FIFO，
// 不按 ID 覆盖记录。

/**
 * @brief 在 FIFO 接纳前校验整个批次；返回零或负 errno。
 */
int SubmitCanRecords(const dust::wire::CanFrame* records, size_t count);
// 丢弃软件队列中的业务流量，仅保留传入的本地停止动作。
// 本接口无法撤回
// 已提交到硬件的帧。

/**
 * @brief 丢弃排队的业务流量并接纳保护记录。
 * @note 已提交到硬件的帧无法撤回。
 */
int ReplaceCanTraffic(const dust::wire::CanFrame* records, size_t count);

/**
 * @brief 返回总线传输和控制器计数器；无效总线返回全零计数。
 */
CanStats ReadCanStats(uint8_t bus);

/**
 * @brief 以原子操作读取并清除锁存的 CAN 输出故障。
 */
bool TakeCanFault();
}  // namespace platform

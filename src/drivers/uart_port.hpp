/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include "usb_protocol.hpp"
namespace gateway {

// 能力位：UART10=2、UART2=4、UART3=16、UART11=32、UART12=64、UART15=128。

/**
 * @brief 启动固定配置的 UART 端口；返回能力位
 * UART10=2、UART2=4、UART3=16、UART11=32、UART12=64、UART15=128。
 */
uint32_t StartUartPorts();

/**
 * @brief 以原子操作读取并清除锁存的 UART 输出故障。
 */
bool TakeUartFault();

/**
 * @brief 返回指定逻辑 UART 通道是否已初始化。
 */
bool UartAvailable(uint8_t channel);

/**
 * @brief 将 UART 载荷复制到有界输出队列。
 * @return 接纳成功返回零；参数无效、设备不可用或队列已满时返回负 errno。
 */
int SubmitUart(uint8_t channel, const uint8_t* data, size_t length);

/**
 * @brief 丢弃软件队列中的 UART 写入；已启动的硬件传输无法撤回。
 */
void DiscardUartWrites();

/**
 * @brief 各端口收发计数器快照。
 */
struct UartStats {
    uint32_t received, dropped, tx_completed, tx_errors;
};

/**
 * @brief 返回通道计数器快照；未知通道返回全零计数。
 */
UartStats ReadUartStats(uint8_t channel);
}  // namespace gateway

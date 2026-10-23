/*
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file usb_session.hpp
 * @brief 管理 USB 通信会话及其收发状态。
 * @details 这是业务层可见的平台边界。返回负 errno
 * 表示参数、设备或传输失败；调用方不得绕过该接口直接依赖具体驱动实例。
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace platform {

/** 传输层诊断，不包含上层协议或请求/响应匹配。时间戳使用单调时钟微秒低 32 位。
 */
struct UsbSessionStats {
    uint32_t received_packets = 0;
    uint32_t submitted_packets = 0;
    uint32_t completed_packets = 0;
    uint32_t receive_queue_drops = 0;
    uint32_t endpoint_errors = 0;
    uint32_t last_receive_us = 0;
    uint32_t last_complete_us = 0;
};
/**
 * @brief Copy current atomic transport counters into stats; a null pointer is
 * ignored.
 */
void ReadUsbSessionStats(UsbSessionStats* stats);

/**
 * @brief 初始化 USB 设备会话。
 * @param polling true 时不创建通用接收线程；仅支持 raw 模式，由唯一主循环轮询。
 * @return 成功返回 0，参数无效或底层操作失败时返回负 errno 错误码。
 */
int InitializeUsbSession(bool polling = false);
/** Raw transport only: rearm OUT from a single polling owner outside the ISR.
 */
void PollUsbSession();
/**
 * @brief 判断 USB 主机是否已完成设备配置。
 * @return USB 已枚举并配置时返回 `true`。
 */
bool IsUsbConfigured();
/**
 * @brief Return the connection generation used to reject data from an earlier
 * USB connection.
 */
uint32_t UsbConnectionEpoch();
/**
 * @brief 通过当前 USB 会话发送字节序列。
 * @param[in] data 输入字节缓冲区；长度由相邻长度参数给出。
 * @param len 输入缓冲区中的有效字节数。
 * @return 成功返回提交字节数；失败返回负 errno。提交不表示传输完成。
 */
int SendUsb(const uint8_t* data, size_t len);
/** 单发送者模式下等待最近提交的 IN 完成；不得与业务发送 worker 同时使用。 */
int WaitUsbTxComplete(int32_t timeout_ms);
/**
 * @brief 在指定超时时间内接收一帧 USB 数据。
 * @details 回调将数据复制入有界队列，接收线程负责 OUT
 * 重启；调用者必须是唯一消费者。 输出缓冲不足时消费并截断该帧，返回 -EMSGSIZE。
 * @param[out] out 接收结果的输出对象；不得为空。
 * @param capacity 输出缓冲区可容纳的最大字节数。
 * @param[out] out_len 接收实际输出字节数的指针；不得为空。
 * @param timeout_ms 等待超时时间，单位为毫秒；负值表示永久等待。
 * @param receive_timestamp_us 可选输出，OUT 回调单调微秒时间戳低 32 位。
 * @return 成功返回 0；超时、参数无效或设备异常时返回负 errno 错误码。
 */
int ReceiveUsb(uint8_t* out, size_t capacity, size_t* out_len,
               int32_t timeout_ms, uint32_t* receive_timestamp_us = nullptr);

}  // namespace platform

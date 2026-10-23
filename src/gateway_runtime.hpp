/* SPDX-License-Identifier: Apache-2.0 */

/**
 * @file gateway_runtime.hpp
 * @brief 固件运行入口及所有权约定。
 */
#ifndef DUST_ADAPTER_GATEWAY_RUNTIME_HPP_
#define DUST_ADAPTER_GATEWAY_RUNTIME_HPP_
namespace gateway {

/**
 * @brief 初始化硬件并运行由单一线程拥有的会话请求循环。
 * @details
 * 此线程独占租约和保护策略。回调通过有界队列通信，不得修改策略。启动保护先于
 * USB 初始化。
 * @return 初始化失败返回 -1；成功后不返回。
 */
int RunGateway();
}  // namespace gateway
#endif  // DUST_ADAPTER_GATEWAY_RUNTIME_HPP_

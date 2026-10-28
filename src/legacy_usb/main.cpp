/* SPDX-License-Identifier: Apache-2.0 */

/**
 * @file main.cpp
 * @brief Zephyr USB 网关入口。
 */
#include "gateway_runtime.hpp"

/**
 * @brief 运行网关；初始化失败时返回错误码。
 */
int main() {
    return gateway::RunGateway();
}

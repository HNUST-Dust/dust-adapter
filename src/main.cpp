/* SPDX-License-Identifier: Apache-2.0 */
/** @file main.cpp
 * @brief Zephyr USB gateway entry point.
 */
#include "gateway_runtime.hpp"
/**
 * @brief Run the gateway; return its initialization failure code.
 */
int main() { return gateway::RunGateway(); }

/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
namespace platform {
/** @brief 初始化加热 PWM 和温控任务，初始输出为零。 */
int InitializeImuHeater();
/** @brief 提交新采集的 IMU 温度，供温控任务使用。 */
void ObserveImuTemperature(float temperature_c);
/** @brief 读取温控任务发布的锁存故障，不清除故障。 */
bool ImuHeaterFaulted();
}  // namespace platform

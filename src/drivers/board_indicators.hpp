/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
namespace platform {
/** @brief 初始化 LED 和蜂鸣器，初始全部关闭。 */
int InitializeBoardIndicators();
/** @brief 更新指示状态；故障红灯短鸣、已授权绿灯，其余蓝灯。 */
void SetBoardState(bool armed, bool fault);
}  // namespace platform

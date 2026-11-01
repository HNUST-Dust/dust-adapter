/* SPDX-License-Identifier: Apache-2.0 */

/* CAN 硬件驱动：设备初始化、过滤器、原始帧收发与控制器状态。
 * 本层不包含任何 EtherCAT/协议语义。 */

#ifndef DRIVERS_CAN_DRIVER_HPP_
#define DRIVERS_CAN_DRIVER_HPP_

#include <cstddef>
#include <cstdint>

#include <zephyr/device.h>
#include <zephyr/drivers/can.h>

namespace dust::drivers {

struct CanFrame {
    uint32_t id = 0;
    uint8_t length = 0;
    uint8_t data[8]{};
};

struct CanBusConfig {
    const device* dev = nullptr;
    uint8_t index = 0;
    uint32_t bitrate = 0;
};

struct CanFilterConfig {
    uint8_t bus = 0;
    uint32_t id = 0;
    uint32_t mask = 0x7ff;
};

using CanRxCallback = void (*)(void* context, uint8_t bus, const CanFrame& frame);
using CanTxDoneCallback = void (*)(void* context, uint8_t bus, int error);

/* 初始化给定总线（模式/位率/过滤器/启动）。返回首个错误码，0 表示成功。 */
int CanInit(const CanBusConfig* buses, size_t bus_count,
            const CanFilterConfig* filters, size_t filter_count,
            CanRxCallback rx, void* context);

bool CanReady(uint8_t bus);

/* 提交一批帧；完成（成功或失败）时回调 done。同步返回提交结果。 */
int CanSendBatch(uint8_t bus, const CanFrame* frames, size_t count,
                 uint64_t deadline_us, CanTxDoneCallback done, void* context);

/* 返回 0 表示撤销成功；非 0 与 hpm_mcan_abort_tx 语义一致。 */
int CanAbortTx(uint8_t bus);

/* 停止控制器并标记总线不可用。 */
void CanStopController(uint8_t bus);

bool CanGetState(uint8_t bus, can_state& state, can_bus_err_cnt& errors);

}  // namespace dust::drivers

#endif /* DRIVERS_CAN_DRIVER_HPP_ */

/* SPDX-License-Identifier: Apache-2.0 */

/* 组合根：单板静态资源、适配器与 domain 服务的装配位置。
 * 生成头/DTS 别名只在这里出现。 */
#pragma once

#include "adapters/ethercat_ssc/ssc_stack.hpp"
#include "drivers/can/can_bus.hpp"
#include "drivers/spi_imu/imu_source.hpp"
#include "drivers/pwm/outputs.hpp"
#include "drivers/uart/uart_port.hpp"
#include "scheduler.hpp"
#include "domain/can/can_service.hpp"
#include "adapters/ethercat_ssc/feedback.hpp"
#include "adapters/ethercat_ssc/io_coordinator.hpp"
#include "adapters/ethercat_ssc/layout_service.hpp"
#include "lifecycle.hpp"
#include "domain/protection/protection.hpp"
#include "domain/protocol/session.hpp"
#include "domain/uart/uart_service.hpp"

namespace app {

class Composition {
   public:
    Composition();
    int Init();
    void Run();

   private:
    ecat::CanBusPort can_bus_;
    ecat::UartPort uart_port_;
    ecat::ImuSource imu_;
    ecat::ZephyrOutputs outputs_;

    ecat::Session session_;
    ecat::LayoutService layout_;
    ecat::CanService can_;
    ecat::UartService uart_;
    ecat::FeedbackAggregator feedback_;
    ecat::IoCoordinator io_;
    ecat::ProtectionService protection_;

    ecat::SscStack stack_;
    ecat::LifecycleService lifecycle_;
    ecat::Scheduler scheduler_;
};

}  // namespace app

/* SPDX-License-Identifier: Apache-2.0 */

/* 实时循环策略：驱动 SSC 栈、采集传感器、慢任务与等待策略。 */
#pragma once

#include <cstdint>

#include "drivers/pwm/outputs.hpp"
#include "adapters/ethercat_ssc/feedback.hpp"
#include "adapters/ethercat_ssc/io_coordinator.hpp"
#include "lifecycle.hpp"
#include "domain/protocol/session.hpp"
#include "adapters/ethercat_ssc/ssc_stack.hpp"

namespace ecat {

class Scheduler {
   public:
    enum class Step { Idle, Active, Fatal };

    Scheduler(LifecycleService& lifecycle, SscStack& stack,
              Session& session, IoCoordinator& io,
              FeedbackAggregator& feedback, ZephyrOutputs& outputs);

    Step RunOnce();

   private:
    LifecycleService& lifecycle_;
    SscStack& stack_;
    Session& session_;
    IoCoordinator& io_;
    FeedbackAggregator& feedback_;
    ZephyrOutputs& outputs_;
};

}  // namespace ecat

/* SPDX-License-Identifier: Apache-2.0 */

#include "scheduler.hpp"

#include <zephyr/kernel.h>

namespace ecat {
namespace {

uint64_t NowUs() {
    return k_ticks_to_us_floor64(k_uptime_ticks());
}

}  // namespace

Scheduler::Scheduler(LifecycleService& lifecycle, SscStack& stack,
                     Session& session, IoCoordinator& io,
                     FeedbackAggregator& feedback, ZephyrOutputs& outputs)
    : lifecycle_(lifecycle),
      stack_(stack),
      session_(session),
      io_(io),
      feedback_(feedback),
      outputs_(outputs) {}

Scheduler::Step Scheduler::RunOnce() {
    {

        if (!lifecycle_.Step()) {
            return Step::Fatal;
        }
    }
    {

        stack_.Poll(false);
    }
    {

        stack_.Step();
    }
    {

        stack_.Poll();
    }

    /* 传感器/热控与主站是否发布无关；PID 由适配器内部按周期限流。 */
    {

    }
    {

        float temperature = 0;
        uint64_t capture_us = 0;
        if (feedback_.LatestTemperature(temperature, capture_us)) {
            outputs_.ObserveTemperature(temperature, capture_us);
        }
        const uint64_t now = NowUs();
        outputs_.Tick(now);
        if (session_.state() == BusinessState::Protected) {
            outputs_.SafeState();
        }
    }

    (void)io_;
    return lifecycle_.Active() ? Step::Active : Step::Idle;
}

}  // namespace ecat

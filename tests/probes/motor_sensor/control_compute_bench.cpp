// SPDX-License-Identifier: Apache-2.0
#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <sched.h>
#include <sys/mman.h>
#include <time.h>

#include "chassis/chassis_controller.h"

namespace chassis = dust_control::chassis;

static uint64_t NowNs() {
    timespec t{};
    clock_gettime(CLOCK_MONOTONIC, &t);
    return uint64_t(t.tv_sec) * 1000000000 + t.tv_nsec;
}

int main() {
    chassis::ChassisController controller;
    chassis::BalanceController balance;
    chassis::ControllerInputs disabled{};
    chassis::ChassisCycleInput input{};
    input.dt = .001;
    input.imu_fresh = input.feedback_valid = true;
    input.leg.left.length = input.leg.right.length = .20;
    input.leg.left.angle = input.leg.right.angle = 1.5707963267948966;
    input.leg.left.jacobian[0][0] = input.leg.right.jacobian[0][0] = .1;
    input.leg.left.jacobian[1][1] = input.leg.right.jacobian[1][1] = .1;

    cpu_set_t cpu;
    CPU_ZERO(&cpu);
    CPU_SET(3, &cpu);
    sched_param priority{70};
    if (sched_setaffinity(0, sizeof(cpu), &cpu) ||
        sched_setscheduler(0, SCHED_FIFO, &priority) ||
        mlockall(MCL_CURRENT | MCL_FUTURE)) {
        perror("RT setup");
        return 2;
    }

    constexpr uint64_t period = 1000000;
    constexpr unsigned ticks = 20000;
    uint64_t release = NowNs();
    uint64_t wake_max = 0, tick_max = 0, balance_max = 0, response_max = 0;
    uint64_t tick_sum = 0, balance_sum = 0;
    unsigned missed = 0, over_deadline = 0;
    double checksum = 0;
    for (unsigned i = 0; i < ticks; ++i) {
        release += period;
        timespec t{time_t(release / 1000000000), long(release % 1000000000)};
        int result;
        do {
            result = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &t, nullptr);
        } while (result == EINTR);
        if (result) {
            return 3;
        }
        const auto start = NowNs();
        const auto late = start - release;
        wake_max = std::max(wake_max, late);
        missed += late / period;
        release += (late / period) * period;

        (void)controller.Tick(disabled, start / 1000, false);
        const auto after_tick = NowNs();
        input.remote.chassis_x = i % 1000 < 500 ? .2 : 0;
        chassis::ChassisControlOutput output{};
        balance.Update(input, output);
        const auto finish = NowNs();
        checksum += output.physical_wheel_torque.left;

        const auto tick_ns = after_tick - start;
        const auto balance_ns = finish - after_tick;
        tick_sum += tick_ns;
        balance_sum += balance_ns;
        tick_max = std::max(tick_max, tick_ns);
        balance_max = std::max(balance_max, balance_ns);
        response_max = std::max(response_max, finish - release);
        over_deadline += finish - release >= period;
    }
    printf("{\"ticks\":%u,\"missed_releases\":%u,\"over_deadline\":%u,"
           "\"wake_max_us\":%.2f,\"disabled_tick_mean_us\":%.2f,"
           "\"disabled_tick_max_us\":%.2f,\"balance_mean_us\":%.2f,"
           "\"balance_max_us\":%.2f,\"response_max_us\":%.2f,\"checksum\":%.6f}\n",
           ticks, missed, over_deadline, wake_max / 1000.0,
           double(tick_sum) / ticks / 1000, tick_max / 1000.0,
           double(balance_sum) / ticks / 1000, balance_max / 1000.0,
           response_max / 1000.0, checksum);
    return std::isfinite(checksum) ? 0 : 4;
}

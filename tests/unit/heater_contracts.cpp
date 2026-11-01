#include <cassert>
#include <limits>

#include "drivers/heater_pid.hpp"
int main() {
    platform::HeaterPid reference;
    assert(std::fabs(reference.Update(44, 45, 0.02f) - 0.1001f) < 0.00001f);
    platform::HeaterPid pid;
    for (int i = 0; i < 10000; ++i) {
        const float duty = pid.Update(20, 40, 0.02f);
        assert(duty >= 0 && duty <= pid.max_duty);
    }
    assert(pid.Update(45, 40, 0.02f) == 0);
    assert(pid.Update(80, 40, 0.02f) == 0);
    assert(pid.Faulted());
    assert(pid.Update(20, 40, 0.02f) == 0);
    platform::HeaterPid invalid;
    assert(invalid.Update(std::numeric_limits<float>::quiet_NaN(), 40, 0.02f) ==
           0);
    assert(invalid.Faulted());
    platform::HeaterPid stale;
    assert(stale.Update(20, 40, 1.0f) == 0);
    assert(stale.Faulted());
}

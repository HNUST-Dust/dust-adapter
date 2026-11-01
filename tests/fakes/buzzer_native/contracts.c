#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
int64_t fake_time, scheduled_ms;
int schedule_result, pwm_result;
bool fake_ready;
uint32_t pulse;
#include "../../../src/drivers/board_pwm.c"
#include "../../../src/drivers/buzzer.c"
int main(void)
{
    assert(buzzer_init() == -ENODEV);
    assert(buzzer_beep(100) == -ENODEV);
    fake_ready = true;
    assert(buzzer_init() == 0 && pulse == 0);
    assert(buzzer_beep(0) == -EINVAL && buzzer_beep(2001) == -EINVAL);
    assert(buzzer_beep(100) == 0 && pulse == 250000 && scheduled_ms == 100);
    fake_time = 40;
    assert(buzzer_beep(200) == 0);
    fake_time = 100;
    buzzer_timeout(&buzzer_work.work);
    assert(pulse == 250000 && scheduled_ms == 140);
    fake_time = 240;
    buzzer_timeout(&buzzer_work.work);
    assert(pulse == 0);
    assert(buzzer_beep(100) == 0);
    assert(buzzer_stop() == 0 && pulse == 0);
    buzzer_timeout(&buzzer_work.work);
    assert(pulse == 0);
    schedule_result = -EIO;
    assert(buzzer_beep(100) == -EIO && pulse == 0);
    pwm_result = -EIO;
    assert(buzzer_beep(100) == -EIO);
    return 0;
}

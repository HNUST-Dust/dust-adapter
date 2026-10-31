#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include "drivers/buzzer.h"
int main(void)
{
    int rc = dust_buzzer_init();
    if (rc) { printk("buzzer init failed: %d\n", rc); return 1; }
    printk("buzzer: 2 kHz, 100 ms every 2 s\n");
    for (;;) {
        rc = dust_buzzer_beep(100);
        if (rc) { printk("buzzer pulse failed: %d\n", rc); return 1; }
        k_sleep(K_SECONDS(2));
    }
}

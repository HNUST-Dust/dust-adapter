#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/can.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>

static const struct device *const buses[] = {
    DEVICE_DT_GET(DT_NODELABEL(mcan0)), DEVICE_DT_GET(DT_NODELABEL(mcan1)),
    DEVICE_DT_GET(DT_NODELABEL(mcan2)), DEVICE_DT_GET(DT_NODELABEL(mcan3)),
};
static atomic_t frames;
static void receive(const struct device *dev, struct can_frame *frame, void *arg)
{
    ARG_UNUSED(dev); ARG_UNUSED(frame); ARG_UNUSED(arg);
    atomic_inc(&frames);
}
int main(void)
{
    const struct device *dev = buses[CAN_CHANNEL];
    struct can_filter filter = { .id = 0, .mask = 0, .flags = 0 };
    int rc;
    printk("CAN%d passive receive test, 1 Mbps; no transmit or ACK\n", CAN_CHANNEL);
    if (!device_is_ready(dev)) { printk("device unavailable\n"); return 1; }
    rc = can_set_mode(dev, CAN_MODE_LISTENONLY);
    if (!rc) rc = can_set_bitrate(dev, 1000000);
    if (!rc) { int id = can_add_rx_filter(dev, receive, NULL, &filter); if (id < 0) rc = id; }
    if (!rc) rc = can_start(dev);
    if (rc) { printk("initialization failed: %d\n", rc); return 1; }
    for (;;) {
        enum can_state state = CAN_STATE_STOPPED; struct can_bus_err_cnt errors = {0};
        rc = can_get_state(dev, &state, &errors);
        printk("CAN%d frames=%ld state=%d tx_errors=%u rx_errors=%u rc=%d\n",
               CAN_CHANNEL, (long)atomic_get(&frames), (int)state,
               errors.tx_err_cnt, errors.rx_err_cnt, rc);
        k_sleep(K_SECONDS(1));
    }
}

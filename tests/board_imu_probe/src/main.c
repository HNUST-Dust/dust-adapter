#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/printk.h>

static const struct device *const spi = DEVICE_DT_GET(DT_NODELABEL(spi3));
static const struct spi_config config = {
    .frequency = 1000000,
    .operation = SPI_OP_MODE_MASTER | SPI_WORD_SET(8) | SPI_TRANSFER_MSB,
    .slave = 0,
    .cs = { .gpio = GPIO_DT_SPEC_GET(DT_ALIAS(onboard_imu_cs), gpios), .delay = 0 },
};
K_SEM_DEFINE(completed, 0, 1);
static volatile int transfer_status;
static uint8_t tx[32] __aligned(32);
static uint8_t rx[32] __aligned(32);
static struct spi_buf tx_buf = { .buf = tx, .len = 2 };
static struct spi_buf rx_buf = { .buf = rx, .len = 2 };
static struct spi_buf_set tx_set = { .buffers = &tx_buf, .count = 1 };
static struct spi_buf_set rx_set = { .buffers = &rx_buf, .count = 1 };
static void done(const struct device *dev, int result, void *user)
{
    ARG_UNUSED(dev); ARG_UNUSED(user);
    transfer_status = result;
    k_sem_give(&completed);
}
static int read_reg(uint8_t reg, uint8_t *value)
{
    tx[0] = reg | 0x80; tx[1] = 0; rx[0] = rx[1] = 0;
    int rc = spi_transceive_cb(spi, &config, &tx_set, &rx_set, done, NULL);
    if (rc) return rc;
    /* On timeout stop the probe; static buffers remain alive for any late callback. */
    if (k_sem_take(&completed, K_SECONDS(1))) return -ETIMEDOUT;
    rc = transfer_status;
    if (!rc) *value = rx[1];
    return rc;
}
int main(void)
{
    printk("SPI3 ICM-42688-P asynchronous DMA read-only probe; heater off\n");
    if (!device_is_ready(spi) || !gpio_is_ready_dt(&config.cs.gpio)) {
        printk("SPI3 or CS unavailable\n"); return 1;
    }
    for (;;) {
        uint8_t hxy = 0, icm = 0, high = 0, low = 0;
        int rc = read_reg(0x01, &hxy);
        if (!rc) rc = read_reg(0x75, &icm);
        if (!rc) rc = read_reg(0x22, &high);
        if (!rc) rc = read_reg(0x23, &low);
        if (rc) { printk("SPI DMA error %d; stopped\n", rc); return 1; }
        printk("HXY id[01]=%02x (expected 6a); ICM id[75]=%02x (expected 47); HXY temp raw[22:23]=%02x%02x (not initialized)\n", hxy, icm, high, low);
        k_sleep(K_SECONDS(1));
    }
}

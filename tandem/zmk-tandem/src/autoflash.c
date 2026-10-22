/*
 * Tandem automatic flashing.
 *
 * Exposes a dedicated USB CDC ACM port. When the host sets it to 1200 baud,
 * this half reboots into the UF2 bootloader, the same way &bootloader does.
 * When the host writes "info", this half answers with one JSON line so the
 * app can tell which half it is and which build it runs.
 */

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/uart/cdc_acm.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/reboot.h>

#include <dt-bindings/zmk/reset.h>

LOG_MODULE_REGISTER(tandem_autoflash, CONFIG_ZMK_LOG_LEVEL);

#define BOOTLOADER_BAUD 1200

#if IS_ENABLED(CONFIG_ZMK_SPLIT) && !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
#define ROLE "peripheral"
#else
#define ROLE "central"
#endif

static const struct device *const uart = DEVICE_DT_GET(DT_CHOSEN(tandem_autoflash_uart));

static const char info_line[] = "{\"tandem\":1,\"role\":\"" ROLE "\",\"name\":\"" CONFIG_ZMK_KEYBOARD_NAME
                                "\",\"build\":\"" CONFIG_TANDEM_BUILD_ID "\"}\n";

static char rx_line[16];
static size_t rx_len;

static void reboot_work_handler(struct k_work *work) { sys_reboot(RST_UF2); }

static K_WORK_DELAYABLE_DEFINE(reboot_work, reboot_work_handler);

static void info_work_handler(struct k_work *work) {
    for (size_t i = 0; i < sizeof(info_line) - 1; i++) {
        uart_poll_out(uart, info_line[i]);
    }
}

static K_WORK_DEFINE(info_work, info_work_handler);

static void handle_rx_byte(uint8_t c) {
    if (c == '\r' || c == '\n') {
        rx_line[rx_len] = '\0';
        if (strcmp(rx_line, "info") == 0) {
            k_work_submit(&info_work);
        }
        rx_len = 0;
        return;
    }

    if (rx_len < sizeof(rx_line) - 1) {
        rx_line[rx_len++] = c;
    } else {
        // Too long to be a command we know. Drop it.
        rx_len = 0;
    }
}

static void uart_isr(const struct device *dev, void *user_data) {
    while (uart_irq_update(dev) && uart_irq_is_pending(dev)) {
        if (uart_irq_rx_ready(dev)) {
            uint8_t buf[16];
            int len = uart_fifo_read(dev, buf, sizeof(buf));
            for (int i = 0; i < len; i++) {
                handle_rx_byte(buf[i]);
            }
        }
    }
}

static void dte_rate_changed(const struct device *dev, uint32_t rate) {
    if (rate != BOOTLOADER_BAUD) {
        return;
    }

    LOG_INF("Host set %d baud, rebooting into the bootloader", BOOTLOADER_BAUD);
    // Give the USB stack time to finish the control transfer first.
    k_work_schedule(&reboot_work, K_MSEC(50));
}

static int tandem_autoflash_init(void) {
    if (!device_is_ready(uart)) {
        LOG_ERR("Tandem serial port is not ready");
        return -ENODEV;
    }

    int err = cdc_acm_dte_rate_callback_set(uart, dte_rate_changed);
    if (err) {
        LOG_ERR("Could not watch baud rate changes (%d)", err);
        return err;
    }

    uart_irq_callback_set(uart, uart_isr);
    uart_irq_rx_enable(uart);
    return 0;
}

SYS_INIT(tandem_autoflash_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

/*
 * Tandem automatic flashing.
 *
 * Exposes a dedicated USB CDC ACM port. When the host sets it to 1200 baud,
 * this half reboots into the UF2 bootloader, the same way &bootloader does.
 * The host can also write one of these commands, each ending in a newline:
 *
 *   info        one JSON line: protocol version, role, name, build ID and
 *               repo. On the left half, also whether the right half is
 *               connected to it.
 *   keymap      a JSON line with the byte count, then the keymap source itself
 *   boot right  left half only: reboots the right half into its bootloader
 *               over the split link, as &bootloader pressed on that half
 *               would. Answers one JSON line saying whether it was sent. This
 *               way the host needs no port of the right half's own to update
 *               it, only the bootloader's.
 */

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/uart/cdc_acm.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/sys/util.h>

#include <dt-bindings/zmk/reset.h>

LOG_MODULE_REGISTER(tandem_autoflash, CONFIG_ZMK_LOG_LEVEL);

#define BOOTLOADER_BAUD 1200
#define PROTOCOL_VERSION "3"

#if IS_ENABLED(CONFIG_ZMK_SPLIT) && !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
#define ROLE "peripheral"
#else
#define ROLE "central"
#endif

// The left half can restart the right one when it talks to it over Bluetooth
// and the keymap uses &bootloader, which ZMK only builds in when referenced.
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) && IS_ENABLED(CONFIG_ZMK_SPLIT_BLE) && DT_NODE_EXISTS(DT_NODELABEL(bootloader))
#define RELAY 1
#include <zephyr/bluetooth/conn.h>
#include <zmk/behavior.h>
#include <zmk/split/bluetooth/central.h>
#else
#define RELAY 0
#endif

static const struct device *const uart = DEVICE_DT_GET(DT_CHOSEN(tandem_autoflash_uart));

// The keymap source, embedded at build time. Always ends in one extra 0x00.
static const uint8_t keymap_text[] = {
#include "tandem_keymap.inc"
};
#define KEYMAP_LEN (sizeof(keymap_text) - 1)

static char rx_line[16];
static size_t rx_len;

static void send(const char *text, size_t len) {
    for (size_t i = 0; i < len; i++) {
        uart_poll_out(uart, text[i]);
    }
}

static void reboot_work_handler(struct k_work *work) { sys_reboot(RST_UF2); }

static K_WORK_DELAYABLE_DEFINE(reboot_work, reboot_work_handler);

#if RELAY
// The left half holds the central role in exactly one connection: the one to
// the right half. Hosts connect to it the other way round. Asking Bluetooth
// beats tracking ZMK's peripheral status events, which also fire, as
// disconnected, when a host drops.
static void count_right(struct bt_conn *conn, void *data) {
    struct bt_conn_info info;
    if (bt_conn_get_info(conn, &info) == 0 && info.role == BT_CONN_ROLE_CENTRAL && info.state == BT_CONN_STATE_CONNECTED) {
        (*(int *)data)++;
    }
}

static bool right_connected(void) {
    int count = 0;
    bt_conn_foreach(BT_CONN_TYPE_LE, count_right, &count);
    return count > 0;
}

static void boot_right_handler(struct k_work *work) {
    struct zmk_behavior_binding binding = {.behavior_dev = DEVICE_DT_NAME(DT_NODELABEL(bootloader))};
    struct zmk_behavior_binding_event event = {.position = 0, .timestamp = k_uptime_get(), .source = 0};
    int err = right_connected() ? zmk_split_bt_invoke_behavior(0, &binding, event, true) : -ENOTCONN;
    if (err) {
        LOG_WRN("Could not restart the right half (%d)", err);
    }

    char line[64];
    int len = snprintk(line, sizeof(line), "{\"tandem\":" PROTOCOL_VERSION ",\"boot\":\"right\",\"ok\":%s}\n", err ? "false" : "true");
    send(line, MIN((size_t)len, sizeof(line) - 1));
}

static K_WORK_DEFINE(boot_right_work, boot_right_handler);
#endif

static void info_work_handler(struct k_work *work) {
    static char line[320];
    const char *right = "";
#if RELAY
    right = right_connected() ? ",\"peripheral\":true" : ",\"peripheral\":false";
#endif
    int len = snprintk(line, sizeof(line),
                       "{\"tandem\":" PROTOCOL_VERSION ",\"role\":\"" ROLE "\",\"name\":\"" CONFIG_ZMK_KEYBOARD_NAME
                       "\",\"build\":\"" CONFIG_TANDEM_BUILD_ID "\",\"repo\":\"" CONFIG_TANDEM_REPO "\"%s}\n",
                       right);
    send(line, MIN((size_t)len, sizeof(line) - 1));
}

static K_WORK_DEFINE(info_work, info_work_handler);

// The keymap is larger than the port's transmit buffer, so it goes out in
// small chunks a few milliseconds apart instead of all at once.
#define KEYMAP_CHUNK 192
static size_t keymap_sent;

static void keymap_work_handler(struct k_work *work);

static K_WORK_DELAYABLE_DEFINE(keymap_work, keymap_work_handler);

static void keymap_work_handler(struct k_work *work) {
    size_t end = MIN(keymap_sent + KEYMAP_CHUNK, KEYMAP_LEN);
    send((const char *)&keymap_text[keymap_sent], end - keymap_sent);
    keymap_sent = end;
    if (keymap_sent < KEYMAP_LEN) {
        k_work_schedule(&keymap_work, K_MSEC(4));
    } else {
        send("\n", 1);
    }
}

static void start_keymap(void) {
    char header[48];
    int len = snprintk(header, sizeof(header), "{\"tandem\":" PROTOCOL_VERSION ",\"keymap\":%u}\n", (unsigned)KEYMAP_LEN);
    send(header, len);
    keymap_sent = 0;
    k_work_schedule(&keymap_work, K_MSEC(4));
}

static void keymap_start_handler(struct k_work *work) { start_keymap(); }

static K_WORK_DEFINE(keymap_start, keymap_start_handler);

static void handle_rx_byte(uint8_t c) {
    if (c == '\r' || c == '\n') {
        rx_line[rx_len] = '\0';
        if (strcmp(rx_line, "info") == 0) {
            k_work_submit(&info_work);
        } else if (strcmp(rx_line, "keymap") == 0) {
            k_work_submit(&keymap_start);
#if RELAY
        } else if (strcmp(rx_line, "boot right") == 0) {
            k_work_submit(&boot_right_work);
#endif
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

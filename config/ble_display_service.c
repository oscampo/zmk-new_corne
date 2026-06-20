#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>
#include <lvgl.h>
#include <string.h>

LOG_MODULE_REGISTER(ble_display_svc, LOG_LEVEL_INF);

#define TEXT_MAX_LEN 64
static char text_buf[TEXT_MAX_LEN + 1];
static lv_obj_t *ticker_label;

/* Runs on system workqueue — safe for LVGL on cooperative nRF52 target */
static void refresh_label_fn(struct k_work *work) {
    if (ticker_label) {
        lv_label_set_text(ticker_label, text_buf);
    }
}
static K_WORK_DEFINE(refresh_work, refresh_label_fn);

/* Add our ticker label to whatever screen is active after display settles */
static void add_ticker_fn(struct k_work *work) {
    lv_obj_t *screen = lv_scr_act();
    if (!screen) {
        return;
    }
    ticker_label = lv_label_create(screen);
    lv_label_set_long_mode(ticker_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(ticker_label, 64);
    lv_label_set_text(ticker_label, "");
    lv_obj_align(ticker_label, LV_ALIGN_BOTTOM_MID, 0, -2);
    if (text_buf[0]) {
        lv_label_set_text(ticker_label, text_buf);
    }
}
static K_WORK_DEFINE(add_ticker_work, add_ticker_fn);

static void ticker_timer_fn(struct k_timer *t) {
    k_work_submit(&add_ticker_work);
}
static K_TIMER_DEFINE(ticker_timer, ticker_timer_fn, NULL);

static int ble_display_init(void) {
    /* Delay so ZMK's display thread has time to create the status screen */
    k_timer_start(&ticker_timer, K_SECONDS(2), K_NO_WAIT);
    return 0;
}
SYS_INIT(ble_display_init, APPLICATION, 99);

/* ── BLE GATT service ──────────────────────────────────────────────────── */

static ssize_t on_ble_write(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                            const void *buf, uint16_t len, uint16_t offset,
                            uint8_t flags) {
    if (offset >= TEXT_MAX_LEN) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }
    uint16_t n = MIN(len, TEXT_MAX_LEN - offset);
    memcpy(text_buf + offset, buf, n);
    text_buf[offset + n] = '\0';
    LOG_INF("BLE display text: %s", text_buf);
    k_work_submit(&refresh_work);
    return n;
}

/*
 * Service UUID:        00001523-1212-efde-1523-785feabcd123
 * Characteristic UUID: 00001524-1212-efde-1523-785feabcd123
 */
#define KBD_DISPLAY_SVC_UUID \
    BT_UUID_DECLARE_128(BT_UUID_128_ENCODE(0x00001523, 0x1212, 0xefde, \
                                           0x1523, 0x785feabcd123ULL))
#define KBD_DISPLAY_CHAR_UUID \
    BT_UUID_DECLARE_128(BT_UUID_128_ENCODE(0x00001524, 0x1212, 0xefde, \
                                           0x1523, 0x785feabcd123ULL))

BT_GATT_SERVICE_DEFINE(keyboard_display_svc,
    BT_GATT_PRIMARY_SERVICE(KBD_DISPLAY_SVC_UUID),
    BT_GATT_CHARACTERISTIC(KBD_DISPLAY_CHAR_UUID,
        BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP,
        BT_GATT_PERM_WRITE,
        NULL, on_ble_write, NULL),
);

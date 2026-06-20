#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/logging/log.h>
#include <lvgl.h>
#include <string.h>

#include <zmk/display/widgets/battery_status.h>
#include <zmk/display/widgets/output_status.h>
#include <zmk/display/widgets/layer_status.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/* ── BLE display service ────────────────────────────────────────────────── */

#define TEXT_MAX_LEN 64
static char text_buf[TEXT_MAX_LEN + 1];
static lv_obj_t *display_label;

static void refresh_label_fn(struct k_work *work) {
    if (display_label) {
        lv_label_set_text(display_label, text_buf);
    }
}
static K_WORK_DEFINE(refresh_work, refresh_label_fn);

static ssize_t on_ble_write(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                            const void *buf, uint16_t len, uint16_t offset,
                            uint8_t flags) {
    if (offset >= TEXT_MAX_LEN) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }
    uint16_t n = MIN(len, TEXT_MAX_LEN - offset);
    memcpy(text_buf + offset, buf, n);
    text_buf[offset + n] = '\0';
    k_work_submit(&refresh_work);
    return n;
}

/*
 * Custom BLE GATT service for sending arbitrary text to the keyboard display.
 *
 * Service UUID:        00001523-1212-efde-1523-785feabcd123
 * Characteristic UUID: 00001524-1212-efde-1523-785feabcd123
 */
#define KBD_DISPLAY_SVC_UUID \
    BT_UUID_DECLARE_128(BT_UUID_128_ENCODE(0x00001523, 0x1212, 0xefde, 0x1523, 0x785feabcd123ULL))

#define KBD_DISPLAY_CHAR_UUID \
    BT_UUID_DECLARE_128(BT_UUID_128_ENCODE(0x00001524, 0x1212, 0xefde, 0x1523, 0x785feabcd123ULL))

BT_GATT_SERVICE_DEFINE(keyboard_display_svc,
    BT_GATT_PRIMARY_SERVICE(KBD_DISPLAY_SVC_UUID),
    BT_GATT_CHARACTERISTIC(KBD_DISPLAY_CHAR_UUID,
        BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP,
        BT_GATT_PERM_WRITE,
        NULL, on_ble_write, NULL),
);

/* ── Status screen layout ───────────────────────────────────────────────── */

/*
 * Layout for nice!view in portrait mode (68 x 160 px):
 *
 *  ┌──────────────────────────┐
 *  │ [USB/BLE]    [BAT  80%] │  ← top bar
 *  │                          │
 *  │        LAYER NAME        │  ← center (large)
 *  │                          │
 *  │         WPM: 85          │  ← below center (if WPM enabled)
 *  │                          │
 *  │  texto BLE scrolling...  │  ← bottom ticker
 *  └──────────────────────────┘
 */

static struct zmk_widget_battery_status battery_widget;
static struct zmk_widget_output_status  output_widget;
static struct zmk_widget_layer_status   layer_widget;

lv_obj_t *zmk_display_status_screen() {
    lv_obj_t *screen = lv_obj_create(NULL);

    zmk_widget_output_status_init(&output_widget, screen);
    lv_obj_align(zmk_widget_output_status_obj(&output_widget),
                 LV_ALIGN_TOP_LEFT, 2, 2);

    zmk_widget_battery_status_init(&battery_widget, screen);
    lv_obj_align(zmk_widget_battery_status_obj(&battery_widget),
                 LV_ALIGN_TOP_RIGHT, -2, 2);

    zmk_widget_layer_status_init(&layer_widget, screen);
    lv_obj_align(zmk_widget_layer_status_obj(&layer_widget),
                 LV_ALIGN_CENTER, 0, -15);

    display_label = lv_label_create(screen);
    lv_label_set_long_mode(display_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(display_label, 64);
    lv_label_set_text(display_label, "");
    lv_obj_align(display_label, LV_ALIGN_BOTTOM_MID, 0, -2);

    return screen;
}

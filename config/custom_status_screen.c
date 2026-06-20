#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/logging/log.h>
#include <lvgl.h>
#include <string.h>

#if IS_ENABLED(CONFIG_ZMK_WIDGET_BATTERY_STATUS)
#include <zmk/display/widgets/battery_status.h>
static struct zmk_widget_battery_status battery_widget;
#endif

#if IS_ENABLED(CONFIG_ZMK_WIDGET_OUTPUT_STATUS)
#include <zmk/display/widgets/output_status.h>
static struct zmk_widget_output_status output_widget;
#endif

#if IS_ENABLED(CONFIG_ZMK_WIDGET_LAYER_STATUS)
#include <zmk/display/widgets/layer_status.h>
static struct zmk_widget_layer_status layer_widget;
#endif

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/* ── BLE text ticker ───────────────────────────────────────────────────── */

#define TEXT_MAX_LEN 64
static char text_buf[TEXT_MAX_LEN + 1];
static lv_obj_t *ble_label;

static void refresh_ble_label(struct k_work *work) {
    if (ble_label) {
        lv_label_set_text(ble_label, text_buf);
    }
}
static K_WORK_DEFINE(refresh_work, refresh_ble_label);

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
    k_work_submit(&refresh_work);
    return n;
}

/*
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
        BT_GATT_PERM_WRITE, NULL, on_ble_write, NULL),
);

/* ── Status screen layout ──────────────────────────────────────────────── */
/*
 * Display: 68 × 160 px (portrait, nice!view)
 *
 *  ┌──────────┐  y=0
 *  │  QWERTY  │  layer name + circles  (~56 px)
 *  │  ○ ① ② │
 *  ├──────────┤  y=56
 *  │          │  BLE text area  (~88 px)
 *  │ text...  │  (scrolling, where the WPM graph used to be)
 *  │          │
 *  ├──────────┤  y=144
 *  │ BT  BAT  │  output + battery icons  (~16 px)
 *  └──────────┘  y=160
 */

lv_obj_t *zmk_display_status_screen() {
    lv_obj_t *screen = lv_obj_create(NULL);

#if IS_ENABLED(CONFIG_ZMK_WIDGET_LAYER_STATUS)
    zmk_widget_layer_status_init(&layer_widget, screen);
    lv_obj_align(zmk_widget_layer_status_obj(&layer_widget), LV_ALIGN_TOP_MID, 0, 2);
#endif

#if IS_ENABLED(CONFIG_ZMK_WIDGET_OUTPUT_STATUS)
    zmk_widget_output_status_init(&output_widget, screen);
    lv_obj_align(zmk_widget_output_status_obj(&output_widget), LV_ALIGN_BOTTOM_LEFT, 2, -2);
#endif

#if IS_ENABLED(CONFIG_ZMK_WIDGET_BATTERY_STATUS)
    zmk_widget_battery_status_init(&battery_widget, screen);
    lv_obj_align(zmk_widget_battery_status_obj(&battery_widget), LV_ALIGN_BOTTOM_RIGHT, -2, -2);
#endif

    /* BLE text — occupies the large center area freed by removing the WPM graph */
    ble_label = lv_label_create(screen);
    lv_label_set_long_mode(ble_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(ble_label, 64);
    lv_label_set_text(ble_label, "");
    lv_obj_align(ble_label, LV_ALIGN_CENTER, 0, 20);

    return screen;
}

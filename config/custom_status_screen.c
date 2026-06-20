#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/init.h>
#include <lvgl.h>
#include <string.h>

/* ── BLE text buffer ──────────────────────────────────────────────────────── */

#define TEXT_MAX_LEN 64
static char text_buf[TEXT_MAX_LEN + 1];
static lv_obj_t *ble_label;

static void refresh_ble_label(struct k_work *work) {
    if (ble_label) {
        lv_label_set_text(ble_label, text_buf);
    }
}
static K_WORK_DEFINE(refresh_work, refresh_ble_label);

/* ── BLE GATT service ──────────────────────────────────────────────────────── */

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

/* ── Overlay label on the nice_view screen ────────────────────────────────── */
/*
 * We do NOT replace zmk_display_status_screen(). The nice_view shield owns
 * that function and handles rotation, fonts, and layout correctly.
 * We add a text label on top of the WPM graph area after display init.
 *
 * nice_view: 68 x 160 px portrait. WPM graph is at the bottom ~60 px.
 */

static void add_ble_label_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(add_ble_label_work, add_ble_label_fn);

static void add_ble_label_fn(struct k_work *work) {
    lv_obj_t *screen = lv_scr_act();
    if (!screen) {
        k_work_schedule(&add_ble_label_work, K_MSEC(500));
        return;
    }

    ble_label = lv_label_create(screen);
    lv_label_set_long_mode(ble_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(ble_label, 64);
    lv_obj_align(ble_label, LV_ALIGN_BOTTOM_MID, 0, -2);
    lv_obj_set_style_bg_opa(ble_label, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(ble_label, lv_color_black(), 0);
    lv_obj_set_style_text_color(ble_label, lv_color_white(), 0);
    lv_label_set_text(ble_label, "");
}

static int kbd_ble_display_init(void) {
    k_work_schedule(&add_ble_label_work, K_MSEC(2000));
    return 0;
}
SYS_INIT(kbd_ble_display_init, APPLICATION, 99);

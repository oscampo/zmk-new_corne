#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/logging/log.h>
#include <lvgl.h>
#include <string.h>

#include "ble_display_service.h"

LOG_MODULE_REGISTER(ble_display_svc, LOG_LEVEL_INF);

#define TEXT_MAX_LEN 64

static char text_buf[TEXT_MAX_LEN + 1];
static lv_obj_t *display_label;

void ble_display_set_label(lv_obj_t *label) {
    display_label = label;
    if (label && text_buf[0]) {
        lv_label_set_text(label, text_buf);
    }
}

/* k_work runs on the system workqueue — safe to call lv_label_set_text here
   since ZMK's display thread and the workqueue are cooperative on this target. */
static void refresh_label_fn(struct k_work *work) {
    if (display_label) {
        lv_label_set_text(display_label, text_buf);
    }
}
static K_WORK_DEFINE(refresh_work, refresh_label_fn);

static ssize_t on_write(struct bt_conn *conn, const struct bt_gatt_attr *attr,
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
 * Custom BLE GATT service for sending arbitrary text to the keyboard display.
 *
 * Service UUID:        00001523-1212-efde-1523-785feabcd123
 * Characteristic UUID: 00001524-1212-efde-1523-785feabcd123
 *
 * Write up to 64 bytes of UTF-8 text to the characteristic and it will
 * appear on the left-half nice!view display.
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
        NULL, on_write, NULL),
);

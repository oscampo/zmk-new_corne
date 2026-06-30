/*
 * custom_status_screen.c — Dumb display driver for left side (central).
 *
 * The firmware is a pure display sink: it receives 1-bit monochrome bitmap
 * frames from the companion app via BLE and blits them to the display.
 * All rendering (clock, weather, widgets, fonts) happens in the app.
 *
 * GATT service UUID : 00001523-1212-efde-1523-785feabcd123
 * Bitmap char UUID  : 00001525-1212-efde-1523-785feabcd123
 *
 * Frame format (BLE write payload):
 *   [2B offset LE] [2B total_bytes LE] [N bytes bitmap data]
 *   total_bytes is always FRAME_BYTES (1440).
 *   A frame is complete when offset + N == total_bytes.
 *
 * Bitmap format:
 *   68 px wide × 160 px tall, portrait orientation.
 *   1 bit per pixel, row-major, MSB = leftmost pixel.
 *   Each row is byte-aligned: ceil(68/8) = 9 bytes → 9 × 160 = 1440 bytes.
 */

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/drivers/display.h>
#include <zephyr/init.h>
#include <string.h>

/* Only compiled for the left (central) side. */
#if !IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_ROLE_PERIPHERAL)

#define DISPLAY_W   68
#define DISPLAY_H  160
#define ROW_BYTES   9           /* ceil(68/8) */
#define FRAME_BYTES 1440        /* ROW_BYTES × DISPLAY_H */

static uint8_t frame_buf[FRAME_BYTES];

/* ── Display flush ───────────────────────────────────────────────────────── */

static void flush_display(struct k_work *work)
{
    const struct device *disp = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
    if (!device_is_ready(disp)) {
        return;
    }

    struct display_buffer_descriptor desc = {
        .buf_size = FRAME_BYTES,
        .width    = DISPLAY_W,
        .height   = DISPLAY_H,
        .pitch    = DISPLAY_W,
    };
    display_write(disp, 0, 0, &desc, frame_buf);
}

static K_WORK_DEFINE(flush_work, flush_display);

/* ── GATT bitmap characteristic ─────────────────────────────────────────── */

static ssize_t on_bitmap_write(struct bt_conn *conn,
                               const struct bt_gatt_attr *attr,
                               const void *buf, uint16_t len,
                               uint16_t offset, uint8_t flags)
{
    ARG_UNUSED(conn);
    ARG_UNUSED(attr);
    ARG_UNUSED(offset);
    ARG_UNUSED(flags);

    if (len < 4) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }

    const uint8_t *p      = (const uint8_t *)buf;
    uint16_t chunk_offset = (uint16_t)(p[0] | (p[1] << 8));
    uint16_t total        = (uint16_t)(p[2] | (p[3] << 8));
    const uint8_t *data   = p + 4;
    uint16_t data_len     = len - 4;

    if (total != FRAME_BYTES) {
        return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
    }
    if (chunk_offset + data_len > FRAME_BYTES) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }

    memcpy(frame_buf + chunk_offset, data, data_len);

    if (chunk_offset + data_len == FRAME_BYTES) {
        k_work_submit(&flush_work);
    }

    return (ssize_t)len;
}

#define KBD_DISPLAY_SVC_UUID \
    BT_UUID_DECLARE_128(BT_UUID_128_ENCODE( \
        0x00001523, 0x1212, 0xefde, 0x1523, 0x785feabcd123ULL))

#define KBD_BITMAP_CHAR_UUID \
    BT_UUID_DECLARE_128(BT_UUID_128_ENCODE( \
        0x00001525, 0x1212, 0xefde, 0x1523, 0x785feabcd123ULL))

BT_GATT_SERVICE_DEFINE(keyboard_display_svc,
    BT_GATT_PRIMARY_SERVICE(KBD_DISPLAY_SVC_UUID),
    BT_GATT_CHARACTERISTIC(KBD_BITMAP_CHAR_UUID,
        BT_GATT_CHRC_WRITE_WITHOUT_RESP,
        BT_GATT_PERM_WRITE, NULL, on_bitmap_write, NULL),
);

/* ── Init ────────────────────────────────────────────────────────────────── */

static int kbd_ble_display_init(void)
{
    const struct device *disp = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
    if (device_is_ready(disp)) {
        display_blanking_off(disp);
    }
    return 0;
}
SYS_INIT(kbd_ble_display_init, APPLICATION, 99);

#endif /* !CONFIG_ZMK_SPLIT_BLE_ROLE_PERIPHERAL */

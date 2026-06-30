/*
 * custom_status_screen.c — Dumb bitmap display for left side (central).
 *
 * The companion app sends 1-bit monochrome frames via BLE GATT. The firmware
 * decodes and rotates each frame into an LVGL canvas that covers the full
 * display. LVGL owns all rendering — no display_write() conflicts.
 *
 * GATT service UUID : 00001523-1212-efde-1523-785feabcd123
 * Bitmap char UUID  : 00001525-1212-efde-1523-785feabcd123
 *
 * BLE frame protocol (write payload):
 *   [2B offset LE] [2B total_bytes LE] [N bytes bitmap data]
 *   Frame complete when: offset + N == FRAME_BYTES (1440)
 *
 * Bitmap format from app:
 *   68px wide × 160px tall, portrait, 1bpp MSB-first, row-major.
 *   Row stride = ceil(68/8) = 9 bytes → 9 × 160 = 1440 bytes total.
 *   Bit=1 → white, Bit=0 → black.
 *
 * LVGL canvas: 160×68 landscape (native LVGL / display driver orientation).
 * Rotation: portrait pixel (px, py) → landscape pixel (159-py, px) [90° CW].
 */

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/init.h>
#include <lvgl.h>
#include <string.h>

#if !IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_ROLE_PERIPHERAL)

/* ── Dimensions ──────────────────────────────────────────────────────────── */

#define SRC_W        68   /* portrait width  (pixels) */
#define SRC_H       160   /* portrait height (pixels) */
#define SRC_STRIDE    9   /* ceil(68/8) bytes per row  */
#define FRAME_BYTES 1440  /* SRC_STRIDE × SRC_H        */

#define DST_W  160        /* LVGL landscape width  */
#define DST_H   68        /* LVGL landscape height */

/* ── Buffers ─────────────────────────────────────────────────────────────── */

static uint8_t    frame_buf[FRAME_BYTES];            /* raw 1bpp from BLE  */
static lv_color_t canvas_buf[DST_W * DST_H];         /* RGB565 for LVGL    */

/* ── LVGL canvas ─────────────────────────────────────────────────────────── */

static lv_obj_t *ble_canvas;
static lv_obj_t *attached_screen;

static void create_canvas(lv_obj_t *screen)
{
    ble_canvas = lv_canvas_create(screen);
    lv_canvas_set_buffer(ble_canvas, canvas_buf, DST_W, DST_H,
                         LV_IMG_CF_TRUE_COLOR);
    lv_obj_set_pos(ble_canvas, 0, 0);
    lv_canvas_fill_bg(ble_canvas, lv_color_black(), LV_OPA_COVER);
    lv_obj_move_foreground(ble_canvas);
    attached_screen = screen;
}

/* ── Decode + rotate ─────────────────────────────────────────────────────── */

static void decode_and_rotate(void)
{
    for (uint16_t py = 0; py < SRC_H; py++) {
        for (uint16_t px = 0; px < SRC_W; px++) {
            uint8_t byte = frame_buf[py * SRC_STRIDE + (px >> 3)];
            int     lit  = (byte >> (7 - (px & 7))) & 1;

            /* 90° CW: portrait(px,py) → landscape(DST_W-1-py, px) */
            uint16_t dx = (DST_W - 1) - py;
            uint16_t dy = px;
            canvas_buf[dy * DST_W + dx] = lit
                ? lv_color_white()
                : lv_color_black();
        }
    }
}

static void flush_canvas(struct k_work *work)
{
    if (!ble_canvas) return;
    decode_and_rotate();
    lv_obj_invalidate(ble_canvas);
}

static K_WORK_DEFINE(flush_work, flush_canvas);

/* ── GATT bitmap characteristic ─────────────────────────────────────────── */

static ssize_t on_bitmap_write(struct bt_conn *conn,
                               const struct bt_gatt_attr *attr,
                               const void *buf, uint16_t len,
                               uint16_t offset, uint8_t flags)
{
    ARG_UNUSED(conn); ARG_UNUSED(attr); ARG_UNUSED(offset); ARG_UNUSED(flags);

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

/* ── Ensure canvas stays on top when ZMK changes screens ────────────────── */

static void ensure_timer_cb(lv_timer_t *timer)
{
    lv_obj_t *screen = lv_scr_act();
    if (!screen) return;

    if (ble_canvas && attached_screen == screen) {
        lv_obj_move_foreground(ble_canvas);
        return;
    }
    if (ble_canvas) {
        lv_obj_del(ble_canvas);
        ble_canvas = NULL;
    }
    create_canvas(screen);
}

/* ── Init ────────────────────────────────────────────────────────────────── */

static void add_canvas_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(add_canvas_work, add_canvas_fn);

static void add_canvas_fn(struct k_work *work)
{
    lv_obj_t *screen = lv_scr_act();
    if (!screen) {
        k_work_schedule(&add_canvas_work, K_MSEC(500));
        return;
    }
    create_canvas(screen);
    lv_timer_create(ensure_timer_cb, 1000, NULL);
}

static int kbd_ble_display_init(void)
{
    k_work_schedule(&add_canvas_work, K_MSEC(3000));
    return 0;
}
SYS_INIT(kbd_ble_display_init, APPLICATION, 99);

#endif /* !CONFIG_ZMK_SPLIT_BLE_ROLE_PERIPHERAL */

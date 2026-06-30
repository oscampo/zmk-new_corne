/*
 * custom_status_screen.c — Dumb bitmap display + system status for left side.
 *
 * GATT service UUID : 00001523-1212-efde-1523-785feabcd123
 *
 * Characteristic 0x1525 — WriteWithoutResponse
 *   Receives 1bpp portrait frames (68×160, 1440 bytes) in chunks:
 *   [2B offset LE][2B total LE][data]
 *   On complete frame: decode + rotate 90° CW into LVGL canvas.
 *
 * Characteristic 0x1526 — Read + Notify, 1 byte
 *   bit 0     : USB HID active (1 = host USB HID connected)
 *   bits [3:1]: active BLE profile index 0–4 (display as 1–5)
 *   bits [7:4]: reserved, always 0
 *   Notifies on BLE connect, profile change, USB state change.
 *   Battery level: use standard BAS 0x180F / 0x2A19 (ZMK provides it).
 */

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/init.h>
#include <lvgl.h>
#include <string.h>

#include <zmk/ble.h>
#include <zmk/usb.h>
#include <zmk/events/ble_active_profile_changed.h>
#include <zmk/events/usb_conn_state_changed.h>

#if !IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_ROLE_PERIPHERAL)

/* ── Bitmap display dimensions ───────────────────────────────────────────── */

#define SRC_W        68
#define SRC_H       160
#define SRC_STRIDE    9   /* ceil(68/8) */
#define FRAME_BYTES 1440  /* SRC_STRIDE × SRC_H */
#define DST_W       160   /* LVGL landscape */
#define DST_H        68

/* ── Buffers ─────────────────────────────────────────────────────────────── */

static uint8_t    frame_buf[FRAME_BYTES];
static lv_color_t canvas_buf[DST_W * DST_H];

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

/* ── Decode 1bpp portrait + rotate 90° CW → RGB565 landscape ────────────── */

static void decode_and_rotate(void)
{
    for (uint16_t py = 0; py < SRC_H; py++) {
        for (uint16_t px = 0; px < SRC_W; px++) {
            uint8_t byte = frame_buf[py * SRC_STRIDE + (px >> 3)];
            int     lit  = (byte >> (7 - (px & 7))) & 1;
            uint16_t dx  = (DST_W - 1) - py;
            uint16_t dy  = px;
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

/* ── Status byte ─────────────────────────────────────────────────────────── */

static uint8_t build_status_byte(void)
{
    uint8_t flags = 0;
#if IS_ENABLED(CONFIG_ZMK_USB)
    if (zmk_usb_get_conn_state() == ZMK_USB_CONN_HID) {
        flags |= BIT(0);
    }
#endif
    flags |= (uint8_t)((zmk_ble_active_profile_index() & 0x07) << 1);
    return flags;
}

/* ── GATT callbacks ──────────────────────────────────────────────────────── */

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

static ssize_t on_status_read(struct bt_conn *conn,
                              const struct bt_gatt_attr *attr,
                              void *buf, uint16_t len, uint16_t offset)
{
    uint8_t status = build_status_byte();
    return bt_gatt_attr_read(conn, attr, buf, len, offset,
                             &status, sizeof(status));
}

static void on_status_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
    if (value == BT_GATT_CCC_NOTIFY) {
        /* Client just subscribed — send current state immediately */
        uint8_t status = build_status_byte();
        bt_gatt_notify(NULL, attr - 1, &status, sizeof(status));
    }
}

/* ── GATT service ────────────────────────────────────────────────────────── */

#define KBD_DISPLAY_SVC_UUID \
    BT_UUID_DECLARE_128(BT_UUID_128_ENCODE( \
        0x00001523, 0x1212, 0xefde, 0x1523, 0x785feabcd123ULL))
#define KBD_BITMAP_CHAR_UUID \
    BT_UUID_DECLARE_128(BT_UUID_128_ENCODE( \
        0x00001525, 0x1212, 0xefde, 0x1523, 0x785feabcd123ULL))
#define KBD_STATUS_CHAR_UUID \
    BT_UUID_DECLARE_128(BT_UUID_128_ENCODE( \
        0x00001526, 0x1212, 0xefde, 0x1523, 0x785feabcd123ULL))

BT_GATT_SERVICE_DEFINE(keyboard_display_svc,
    BT_GATT_PRIMARY_SERVICE(KBD_DISPLAY_SVC_UUID),
    /* 0x1525 — bitmap write (attrs[1]=decl, attrs[2]=value) */
    BT_GATT_CHARACTERISTIC(KBD_BITMAP_CHAR_UUID,
        BT_GATT_CHRC_WRITE_WITHOUT_RESP,
        BT_GATT_PERM_WRITE, NULL, on_bitmap_write, NULL),
    /* 0x1526 — status read+notify (attrs[3]=decl, attrs[4]=value, attrs[5]=CCC) */
    BT_GATT_CHARACTERISTIC(KBD_STATUS_CHAR_UUID,
        BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
        BT_GATT_PERM_READ, on_status_read, NULL, NULL),
    BT_GATT_CCC(on_status_ccc_changed,
        BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
);

/* ── ZMK event listener — notifies 0x1526 on profile/USB change ─────────── */

static int status_event_listener(const zmk_event_t *eh)
{
    uint8_t status = build_status_byte();
    bt_gatt_notify(NULL, &keyboard_display_svc.attrs[4], &status, sizeof(status));
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(kbd_status, status_event_listener);
ZMK_SUBSCRIPTION(kbd_status, zmk_ble_active_profile_changed);
ZMK_SUBSCRIPTION(kbd_status, zmk_usb_conn_state_changed);

/* ── Canvas lifecycle ────────────────────────────────────────────────────── */

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

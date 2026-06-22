#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/init.h>
#include <lvgl.h>
#include <string.h>

/* ── BLE text buffer ──────────────────────────────────────────────────────── */

#define TEXT_MAX_LEN 64
static char text_buf[TEXT_MAX_LEN + 1];

/* ── Canvas overlay ───────────────────────────────────────────────────────── */

/*
 * Coordinate mapping (LVGL 160×68 landscape → physical 68×160 portrait):
 *   physical_x = LVGL_y          (0..67)
 *   physical_y = 159 - LVGL_x   (0..159, 0=top near USB port)
 *
 * WPM area in physical space: y=21..62, x=0..67 (from nice_view source).
 * In LVGL: x=97..138 (42px), y=0..67 (68px).
 *
 * We use a 68(LVGL_w) × 42(LVGL_h) non-square canvas and rotate the
 * OBJECT 90° CW via lv_obj_set_style_transform_angle. This avoids the
 * square-only constraint of lv_canvas_transform.
 *
 * After 90° CW object rotation around pivot (34, 21):
 *   canvas (x, y) → visual LVGL (118 - y, x)   (approx)
 *   canvas x=0..67  → physical x=0..67           (left-right ✓)
 *   canvas y=0..41  → physical y=20..61           (top-bottom ✓)
 *
 * Logical object position (84, 13) chosen so visual lands on WPM area.
 * Text drawn normally (left→right, top→bottom) in the 68×42 buffer —
 * no pixel-level rotation needed; the object transform handles it.
 */
#define CANVAS_W 68
#define CANVAS_H 42

static lv_color_t ble_cbuf[CANVAS_W * CANVAS_H];
static lv_obj_t  *ble_canvas;

static void draw_ble_canvas(void) {
    if (!ble_canvas) {
        return;
    }

    lv_obj_move_foreground(ble_canvas);

    lv_canvas_fill_bg(ble_canvas, lv_color_black(), LV_OPA_COVER);

    lv_draw_label_dsc_t dsc;
    lv_draw_label_dsc_init(&dsc);
    dsc.color = lv_color_white();

    lv_canvas_draw_text(ble_canvas, 0, 0, CANVAS_W, &dsc, text_buf);
}

static void refresh_ble_canvas(struct k_work *work) {
    draw_ble_canvas();
}
static K_WORK_DEFINE(refresh_work, refresh_ble_canvas);

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

/* ── Add canvas to the nice_view screen ───────────────────────────────────── */

static void add_ble_canvas_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(add_ble_canvas_work, add_ble_canvas_fn);

static void add_ble_canvas_fn(struct k_work *work) {
    lv_obj_t *screen = lv_scr_act();
    if (!screen) {
        k_work_schedule(&add_ble_canvas_work, K_MSEC(500));
        return;
    }

    ble_canvas = lv_canvas_create(screen);
    lv_canvas_set_buffer(ble_canvas, ble_cbuf, CANVAS_W, CANVAS_H,
                         LV_IMG_CF_TRUE_COLOR);

    /*
     * Rotate the canvas OBJECT 90° CW. Combined with the display driver's
     * 90° CCW rotation, the net result is 0° — text appears upright on the
     * physical portrait display without any pixel-level canvas transform.
     * Pivot at canvas center (CANVAS_W/2, CANVAS_H/2) = (34, 21).
     * Logical position (84, 13) places the visual result over WPM area.
     */
    lv_obj_set_style_transform_angle(ble_canvas, 900, 0);
    lv_obj_set_style_transform_pivot_x(ble_canvas, CANVAS_W / 2, 0);
    lv_obj_set_style_transform_pivot_y(ble_canvas, CANVAS_H / 2, 0);
    lv_obj_set_pos(ble_canvas, 84, 13);

    draw_ble_canvas();
}

static int kbd_ble_display_init(void) {
    k_work_schedule(&add_ble_canvas_work, K_MSEC(3000));
    return 0;
}
SYS_INIT(kbd_ble_display_init, APPLICATION, 99);

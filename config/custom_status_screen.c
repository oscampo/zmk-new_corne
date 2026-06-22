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
 * ZMK nice_view coordinate system (from status.c source):
 *   - LVGL display: 160×68 landscape
 *   - Status widget container: lv_obj_set_size(widget->obj, 160, 68)
 *   - "top" canvas: lv_obj_align(top, LV_ALIGN_TOP_RIGHT, 0, 0)
 *     → sits at LVGL x=92..159, y=0..67
 *   - WPM box inside top canvas (pre-rotation):
 *       canvas_draw_rect(canvas, 0, 21, 68, 42, ...) → x=0..67, y=21..62
 *   - rotate_canvas() uses:
 *       lv_canvas_transform(canvas, &img, 900, LV_IMG_ZOOM_NONE,
 *                           -1, 0, CANVAS_SIZE/2, CANVAS_SIZE/2, true)
 *
 * Our overlay mirrors the top canvas exactly:
 *   - Same size (CANVAS_SIZE × CANVAS_SIZE = 68×68)
 *   - Same position (LV_ALIGN_TOP_RIGHT, 0, 0)
 *   - Same rotation parameters
 *   - Text drawn at pre-rotation y=21 (top of WPM box)
 *
 * Trade-off: the overlay covers the battery/USB area (pre-rotation y=0..20,
 * which after rotation maps to LVGL x=139..159). BT circles and layer name
 * remain visible (they are in the middle/bottom canvases at LVGL x=24..91
 * and x=0..23 respectively).
 */
#define CANVAS_SIZE 68

static lv_color_t ble_cbuf[CANVAS_SIZE * CANVAS_SIZE];
static lv_obj_t  *ble_canvas;

static void draw_ble_canvas(void) {
    if (!ble_canvas) {
        return;
    }

    lv_canvas_fill_bg(ble_canvas, lv_color_black(), LV_OPA_COVER);

    lv_draw_label_dsc_t dsc;
    lv_draw_label_dsc_init(&dsc);
    dsc.color = lv_color_white();

    /* Draw at pre-rotation y=21: top edge of the WPM box (height=42 available) */
    lv_canvas_draw_text(ble_canvas, 0, 21, CANVAS_SIZE, &dsc, text_buf);

    /* Rotate 90° CW — identical parameters to nice_view's rotate_canvas() */
    static lv_color_t cbuf_tmp[CANVAS_SIZE * CANVAS_SIZE];
    memcpy(cbuf_tmp, ble_cbuf, sizeof(cbuf_tmp));
    lv_img_dsc_t img;
    img.data       = (const uint8_t *)cbuf_tmp;
    img.header.cf  = LV_IMG_CF_TRUE_COLOR;
    img.header.w   = CANVAS_SIZE;
    img.header.h   = CANVAS_SIZE;
    lv_canvas_transform(ble_canvas, &img, 900, LV_IMG_ZOOM_NONE,
                        -1, 0, CANVAS_SIZE / 2, CANVAS_SIZE / 2, true);
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
    lv_canvas_set_buffer(ble_canvas, ble_cbuf, CANVAS_SIZE, CANVAS_SIZE,
                         LV_IMG_CF_TRUE_COLOR);

    /* Same alignment as nice_view's "top" canvas in the 160×68 LVGL display */
    lv_obj_align(ble_canvas, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_move_foreground(ble_canvas);

    lv_canvas_fill_bg(ble_canvas, lv_color_black(), LV_OPA_COVER);
}

static int kbd_ble_display_init(void) {
    k_work_schedule(&add_ble_canvas_work, K_MSEC(2000));
    return 0;
}
SYS_INIT(kbd_ble_display_init, APPLICATION, 99);

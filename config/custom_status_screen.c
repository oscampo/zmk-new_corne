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
 * Coordinate mapping (LVGL 160×68 → physical 68×160 portrait):
 *   physical_x = LVGL_y         (0..67)
 *   physical_y = 159 - LVGL_x  (0=top/USB, 159=bottom)
 *
 * WPM area: physical y=21..62 → LVGL x=97..138 (42px), y=0..67 (68px).
 *
 * Approach: 68×68 square canvas (lv_canvas_transform works on squares) placed
 * inside a transparent 42×68 clip container that limits visibility to the WPM
 * area only. The canvas is offset -26px horizontally inside the clip so the
 * WPM portion of the rotated canvas aligns with the clip window.
 *
 * Clip container: LVGL x=97..138, y=0..67 → physical y=21..62, x=0..67 ✓
 * Canvas inside clip: position (-26, 0) → absolute LVGL x=71..138
 *   canvas_x=26..67 (the part inside the clip) → after CW90 → pre_y=0..41
 *   canvas_x=0..25  (the BT circles portion)   → clipped away ✓
 * Battery area (LVGL x=139..159): not covered → visible from nice_view ✓
 */
#define CANVAS_SIZE 68

static lv_color_t ble_cbuf[CANVAS_SIZE * CANVAS_SIZE];
static lv_obj_t  *ble_clip;
static lv_obj_t  *ble_canvas;

static void draw_ble_canvas(void) {
    if (!ble_canvas || !ble_clip) {
        return;
    }

    /* Stay on top of nice_view canvases */
    lv_obj_move_foreground(ble_clip);

    lv_canvas_fill_bg(ble_canvas, lv_color_black(), LV_OPA_COVER);

    lv_draw_label_dsc_t dsc;
    lv_draw_label_dsc_init(&dsc);
    dsc.color = lv_color_white();

    /* pre_y=0: maps to canvas_x=67 → physical y=21 (top of WPM area) */
    lv_canvas_draw_text(ble_canvas, 0, 0, CANVAS_SIZE, &dsc, text_buf);

    /* Rotate 90° CW — same as nice_view's rotate_canvas() */
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

    /*
     * Transparent clip container sized to the WPM area.
     * LVGL children are clipped to the parent's content area by default,
     * so the canvas (positioned -26px inside) only shows its WPM portion.
     */
    ble_clip = lv_obj_create(screen);
    lv_obj_set_pos(ble_clip, 97, 0);
    lv_obj_set_size(ble_clip, 42, 68);
    lv_obj_set_style_bg_opa(ble_clip, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ble_clip, 0, 0);
    lv_obj_set_style_pad_all(ble_clip, 0, 0);
    lv_obj_set_style_radius(ble_clip, 0, 0);
    lv_obj_set_scrollbar_mode(ble_clip, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(ble_clip, LV_OBJ_FLAG_OVERFLOW_VISIBLE);

    /* Canvas as child of clip; offset -26 so its WPM portion fills the clip */
    ble_canvas = lv_canvas_create(ble_clip);
    lv_canvas_set_buffer(ble_canvas, ble_cbuf, CANVAS_SIZE, CANVAS_SIZE,
                         LV_IMG_CF_TRUE_COLOR);
    lv_obj_set_pos(ble_canvas, -26, 0);

    draw_ble_canvas();
}

static int kbd_ble_display_init(void) {
    k_work_schedule(&add_ble_canvas_work, K_MSEC(3000));
    return 0;
}
SYS_INIT(kbd_ble_display_init, APPLICATION, 99);

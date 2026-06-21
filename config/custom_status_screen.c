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
 * The nice_view display is physically landscape (160×68 px) but mounted
 * sideways. The shield's widgets use a 68×68 lv_canvas + lv_canvas_transform
 * at 900 (90°) to render glyphs upright. We replicate that approach here so
 * our BLE text appears with the same orientation as the layer label.
 *
 * LVGL canvas coords (before transform): 68 wide × 68 tall
 *   After 90° CW transform the canvas occupies 68×68 on screen.
 * We position the canvas to cover the WPM graph area.
 */
#define CANVAS_SIZE 68

static lv_color_t ble_cbuf[CANVAS_SIZE * CANVAS_SIZE];
static lv_obj_t  *ble_canvas;

static void draw_ble_canvas(void) {
    if (!ble_canvas) {
        return;
    }

    /* Clear to black */
    lv_canvas_fill_bg(ble_canvas, lv_color_black(), LV_OPA_COVER);

    /* Draw text */
    lv_draw_label_dsc_t dsc;
    lv_draw_label_dsc_init(&dsc);
    dsc.color = lv_color_white();

    lv_canvas_draw_text(ble_canvas, 2, 2, CANVAS_SIZE - 4, &dsc, text_buf);

    /* Rotate 90° CW — same as nice_view rotate_canvas() */
    static lv_color_t cbuf_tmp[CANVAS_SIZE * CANVAS_SIZE];
    memcpy(cbuf_tmp, ble_cbuf, sizeof(cbuf_tmp));
    lv_img_dsc_t img;
    img.data = (const uint8_t *)cbuf_tmp;
    img.header.cf = LV_IMG_CF_TRUE_COLOR;
    img.header.w  = CANVAS_SIZE;
    img.header.h  = CANVAS_SIZE;
    /* offset_x=-1 ensures destination pixels map to valid source coords after
     * 90° rotation of a square canvas (same convention as nice_view util.c) */
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

    /*
     * In LVGL portrait space (68 wide × 160 tall):
     *   x=0 is display left edge, y=0 is top.
     *   WPM graph occupies roughly y=60..128 (center band).
     * Place the 68×68 canvas centered vertically in that band.
     */
    /*
     * WPM graph occupies roughly physical x=25..115 on the 160px-wide display.
     * In LVGL portrait (68w×160h), physical x maps to LVGL y.
     * Center of WPM area (physical x≈70) → LVGL y=70, offset from screen center
     * (y=80) = -10.
     */
    lv_obj_align(ble_canvas, LV_ALIGN_CENTER, 0, -10);

    lv_canvas_fill_bg(ble_canvas, lv_color_black(), LV_OPA_COVER);
}

static int kbd_ble_display_init(void) {
    k_work_schedule(&add_ble_canvas_work, K_MSEC(2000));
    return 0;
}
SYS_INIT(kbd_ble_display_init, APPLICATION, 99);

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/init.h>
#include <lvgl.h>
#include <string.h>
#include <stdio.h>

/* ── BLE text / time buffer ───────────────────────────────────────────────── */

#define TEXT_MAX_LEN 64
static char text_buf[TEXT_MAX_LEN + 1];  /* "" = show clock if synced */

/*
 * Clock sync: the host sends "T:<unix_seconds>" via BLE to set the time.
 * We store the Unix timestamp and the uptime at sync so we can compute the
 * current time without an RTC: current_unix = sync_unix + (uptime - sync_uptime) / 1000
 */
static int64_t  clock_sync_unix_s;   /* Unix seconds at last sync (0 = not synced) */
static int64_t  clock_sync_uptime_ms;

static int64_t current_unix_seconds(void) {
    if (clock_sync_unix_s == 0) {
        return 0;
    }
    int64_t elapsed_ms = k_uptime_get() - clock_sync_uptime_ms;
    return clock_sync_unix_s + elapsed_ms / 1000;
}

/* ── Canvas overlay ───────────────────────────────────────────────────────── */

/*
 * Coordinate mapping (LVGL 160×68 → physical 68×160 portrait):
 *   physical_x = LVGL_y         (0..67)
 *   physical_y = 159 - LVGL_x  (0=top/USB, 159=bottom)
 *
 * WPM area: physical y=21..62 → LVGL x=97..138 (42px), y=0..67 (68px).
 *
 * Pre-rotation canvas (68×68): text drawn left→right, top→bottom.
 * After 90° CW rotation: pre_y=0..41 maps to physical_y=21..62 (visible).
 * Montserrat 20 (20px tall, ~12px/char): "HH:MM" = 5×12=60px ≤ 68px ✓
 *                                         height 20px ≤ 41px visible ✓
 */
#define CANVAS_SIZE 68

static lv_color_t ble_cbuf[CANVAS_SIZE * CANVAS_SIZE];
static lv_obj_t  *ble_clip;
static lv_obj_t  *ble_canvas;
static lv_obj_t  *ble_attached_screen;

static void create_ble_overlay(lv_obj_t *screen) {
    ble_clip = lv_obj_create(screen);
    lv_obj_set_pos(ble_clip, 97, 0);
    lv_obj_set_size(ble_clip, 42, 68);
    lv_obj_set_style_bg_opa(ble_clip, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ble_clip, 0, 0);
    lv_obj_set_style_pad_all(ble_clip, 0, 0);
    lv_obj_set_style_radius(ble_clip, 0, 0);
    lv_obj_set_scrollbar_mode(ble_clip, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(ble_clip, LV_OBJ_FLAG_OVERFLOW_VISIBLE);

    ble_canvas = lv_canvas_create(ble_clip);
    lv_canvas_set_buffer(ble_canvas, ble_cbuf, CANVAS_SIZE, CANVAS_SIZE,
                         LV_IMG_CF_TRUE_COLOR);
    lv_obj_set_pos(ble_canvas, -26, 0);

    ble_attached_screen = screen;
}

static void draw_ble_canvas(void) {
    if (!ble_canvas || !ble_clip) {
        return;
    }

    lv_obj_move_foreground(ble_clip);
    lv_canvas_fill_bg(ble_canvas, lv_color_black(), LV_OPA_COVER);

    lv_draw_label_dsc_t dsc;
    lv_draw_label_dsc_init(&dsc);
    dsc.color = lv_color_white();

    bool show_clock = (text_buf[0] == '\0' && clock_sync_unix_s != 0);

    if (show_clock) {
        /* Large font clock: HH:MM centered on the canvas */
        dsc.font = &lv_font_montserrat_20;

        int64_t t = current_unix_seconds();
        int seconds_of_day = (int)(t % 86400);
        int hh = seconds_of_day / 3600;
        int mm = (seconds_of_day % 3600) / 60;

        char clock_str[6];
        snprintf(clock_str, sizeof(clock_str), "%02d:%02d", hh, mm);
        lv_canvas_draw_text(ble_canvas, 0, 0, CANVAS_SIZE, &dsc, clock_str);
    } else if (text_buf[0] != '\0') {
        /* Text mode: default font */
        lv_canvas_draw_text(ble_canvas, 0, 0, CANVAS_SIZE, &dsc, text_buf);
    }
    /* else: no sync yet, show nothing */

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

/*
 * LVGL timer (1s): keeps overlay on the active screen and updates the clock.
 */
static void ensure_overlay_timer_cb(lv_timer_t *timer) {
    lv_obj_t *screen = lv_scr_act();
    if (!screen) {
        return;
    }

    if (ble_clip && ble_attached_screen == screen) {
        lv_obj_move_foreground(ble_clip);
        /* Redraw clock every second when in clock mode */
        if (text_buf[0] == '\0' && clock_sync_unix_s != 0) {
            draw_ble_canvas();
        }
        return;
    }

    /* Screen changed (wake from sleep) — recreate overlay */
    if (ble_clip) {
        lv_obj_del(ble_clip);
        ble_clip   = NULL;
        ble_canvas = NULL;
    }

    create_ble_overlay(screen);
    draw_ble_canvas();
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

    /*
     * Special command: "T:<unix_seconds>" — sync the clock.
     * After parsing, clear text_buf so the display switches to clock mode.
     */
    if (text_buf[0] == 'T' && text_buf[1] == ':') {
        int64_t unix_s = 0;
        const char *p = text_buf + 2;
        while (*p >= '0' && *p <= '9') {
            unix_s = unix_s * 10 + (*p++ - '0');
        }
        if (unix_s > 0) {
            clock_sync_unix_s   = unix_s;
            clock_sync_uptime_ms = k_uptime_get();
        }
        text_buf[0] = '\0';  /* switch to clock display mode */
    }

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

/* ── Startup ──────────────────────────────────────────────────────────────── */

static void add_ble_canvas_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(add_ble_canvas_work, add_ble_canvas_fn);

static void add_ble_canvas_fn(struct k_work *work) {
    lv_obj_t *screen = lv_scr_act();
    if (!screen) {
        k_work_schedule(&add_ble_canvas_work, K_MSEC(500));
        return;
    }

    create_ble_overlay(screen);
    draw_ble_canvas();

    lv_timer_create(ensure_overlay_timer_cb, 1000, NULL);
}

static int kbd_ble_display_init(void) {
    k_work_schedule(&add_ble_canvas_work, K_MSEC(3000));
    return 0;
}
SYS_INIT(kbd_ble_display_init, APPLICATION, 99);

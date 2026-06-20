#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <lvgl.h>

#include <zmk/display/widgets/battery_status.h>
#include <zmk/display/widgets/output_status.h>
#include <zmk/display/widgets/layer_status.h>

#if IS_ENABLED(CONFIG_ZMK_WPM)
#include <zmk/display/widgets/wpm_status.h>
static struct zmk_widget_wpm_status wpm_widget;
#endif

#include "ble_display_service.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/*
 * Layout for nice!view in portrait mode (68 x 160 px):
 *
 *  ┌──────────────────────────┐
 *  │ [USB/BLE]    [BAT  80%] │  ← top bar
 *  │                          │
 *  │        LAYER NAME        │  ← center (large)
 *  │                          │
 *  │         WPM: 85          │  ← below center
 *  │                          │
 *  │  texto BLE scrolling...  │  ← bottom ticker
 *  └──────────────────────────┘
 */

static struct zmk_widget_battery_status battery_widget;
static struct zmk_widget_output_status  output_widget;
static struct zmk_widget_layer_status   layer_widget;

lv_obj_t *zmk_display_status_screen() {
    lv_obj_t *screen = lv_obj_create(NULL);

    /* Output indicator (USB / BLE active profile) — top left */
    zmk_widget_output_status_init(&output_widget, screen);
    lv_obj_align(zmk_widget_output_status_obj(&output_widget),
                 LV_ALIGN_TOP_LEFT, 2, 2);

    /* Battery percentage — top right */
    zmk_widget_battery_status_init(&battery_widget, screen);
    lv_obj_align(zmk_widget_battery_status_obj(&battery_widget),
                 LV_ALIGN_TOP_RIGHT, -2, 2);

    /* Active layer name — center */
    zmk_widget_layer_status_init(&layer_widget, screen);
    lv_obj_align(zmk_widget_layer_status_obj(&layer_widget),
                 LV_ALIGN_CENTER, 0, -15);

#if IS_ENABLED(CONFIG_ZMK_WPM)
    zmk_widget_wpm_status_init(&wpm_widget, screen);
    lv_obj_align(zmk_widget_wpm_status_obj(&wpm_widget),
                 LV_ALIGN_CENTER, 0, 20);
#endif

    /* BLE text ticker — bottom, scrolls if text is longer than display width */
    lv_obj_t *ble_label = lv_label_create(screen);
    lv_label_set_long_mode(ble_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(ble_label, 64);
    lv_label_set_text(ble_label, "");
    lv_obj_align(ble_label, LV_ALIGN_BOTTOM_MID, 0, -2);

    ble_display_set_label(ble_label);

    return screen;
}

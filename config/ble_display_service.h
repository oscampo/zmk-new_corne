#pragma once

#include <lvgl.h>

/* Register the LVGL label that will show text received via BLE.
   Called from custom_status_screen.c during screen initialization. */
void ble_display_set_label(lv_obj_t *label);

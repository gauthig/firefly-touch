/*
 * lv_conf.h for the PC simulator build only.
 *
 * The firmware configures LVGL through Kconfig (sdkconfig.defaults); this
 * file mirrors the display-relevant options so the simulator renders the
 * same way the panel does. Anything not set here falls back to LVGL
 * defaults via lv_conf_internal.h.
 */
#ifndef LV_CONF_H
#define LV_CONF_H

#define LV_COLOR_DEPTH 16          /* match the RGB565 panel */

/* System malloc: the default 64 KB LVGL heap is too small for full-screen
 * snapshots, and there's no reason to emulate the MCU heap on a PC. */
#define LV_USE_STDLIB_MALLOC LV_STDLIB_CLIB
#define LV_USE_STDLIB_STRING LV_STDLIB_CLIB
#define LV_USE_STDLIB_SPRINTF LV_STDLIB_CLIB

#define LV_USE_SDL 1               /* desktop window + mouse-as-touch */

/* Same font set the firmware enables. */
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_24 1
#define LV_FONT_MONTSERRAT_28 1

#define LV_USE_SNAPSHOT 1          /* --shot screenshot mode */

#define LV_USE_LOG 0

#endif /* LV_CONF_H */

#include "ui_theme.h"

lv_style_t ui_style_screen;
lv_style_t ui_style_statusbar;
lv_style_t ui_style_card;
lv_style_t ui_style_card_pressed;

void ui_theme_init(void)
{
    static bool inited = false;
    if (inited) {
        return;
    }
    inited = true;

    lv_style_init(&ui_style_screen);
    lv_style_set_bg_color(&ui_style_screen, UI_COLOR_BG);
    lv_style_set_bg_opa(&ui_style_screen, LV_OPA_COVER);
    lv_style_set_text_color(&ui_style_screen, UI_COLOR_TEXT);
    lv_style_set_border_width(&ui_style_screen, 0);
    lv_style_set_radius(&ui_style_screen, 0);
    lv_style_set_pad_all(&ui_style_screen, 0);

    lv_style_init(&ui_style_statusbar);
    lv_style_set_bg_color(&ui_style_statusbar, UI_COLOR_STATUSBAR);
    lv_style_set_bg_opa(&ui_style_statusbar, LV_OPA_COVER);
    lv_style_set_text_color(&ui_style_statusbar, UI_COLOR_TEXT_DIM);
    lv_style_set_text_font(&ui_style_statusbar, &lv_font_montserrat_16);
    lv_style_set_border_width(&ui_style_statusbar, 0);
    lv_style_set_radius(&ui_style_statusbar, 0);
    lv_style_set_pad_hor(&ui_style_statusbar, 14);
    lv_style_set_pad_ver(&ui_style_statusbar, 0);

    /* Large touch cards: subtle fill, generous radius, no chrome. */
    lv_style_init(&ui_style_card);
    lv_style_set_bg_color(&ui_style_card, UI_COLOR_CARD);
    lv_style_set_bg_opa(&ui_style_card, LV_OPA_COVER);
    lv_style_set_radius(&ui_style_card, 10);
    lv_style_set_border_width(&ui_style_card, 0);
    lv_style_set_shadow_width(&ui_style_card, 0);
    lv_style_set_pad_all(&ui_style_card, 6);
    lv_style_set_text_color(&ui_style_card, UI_COLOR_TEXT);

    lv_style_init(&ui_style_card_pressed);
    lv_style_set_bg_color(&ui_style_card_pressed, UI_COLOR_CARD_PR);
    lv_style_set_bg_opa(&ui_style_card_pressed, LV_OPA_COVER);
}

#include "ui_thermostat.h"

#include <stdio.h>
#include <string.h>

#include "ui_metrics.h"
#include "ui_theme.h"

/* Reasonable clamp for a coach interior setpoint, °F. */
#define SP_MIN_F 45
#define SP_MAX_F 95

/* -/+ taps update the shown setpoint immediately and accumulate; the actual
 * Change is sent once this long after the LAST tap, so mashing "+" five
 * times is one BLE round-trip, not five. */
#define SP_DEBOUNCE_MS 650

/*
 * How long an optimistic (pending) mode/setpoint keeps overriding the
 * status readout before we give up and show whatever the thermostat
 * actually reports. Comfortably longer than one poll + a retry -- the
 * client pulls its poll forward on a submit, so a good round-trip is
 * ~5-10 s; this is the "the command was lost" fallback.
 */
#define PENDING_STALE_MS 25000

/* Modes offered in the picker, in a sensible order. */
static const uint8_t k_modes[] = {
    EASYTOUCH_MODE_OFF, EASYTOUCH_MODE_FAN, EASYTOUCH_MODE_COOL,
    EASYTOUCH_MODE_HEAT, EASYTOUCH_MODE_AQUA_HOT, EASYTOUCH_MODE_AUTO,
};
#define N_MODES (sizeof(k_modes) / sizeof(k_modes[0]))

typedef enum { SP_NONE = 0, SP_COOL, SP_HEAT } sp_field_t;

typedef struct thermo_ctx thermo_ctx_t;

typedef struct {
    thermo_ctx_t *owner;
    uint8_t       index;

    /* Last status for this zone, and whether we have one. */
    easytouch_zone_t last;
    bool             last_valid;

    /* Optimistic overrides: shown (amber) until a status confirms them or
     * PENDING_STALE_MS elapses. */
    bool     mode_dirty;
    uint8_t  pending_mode;
    uint32_t mode_dirty_ms;
    bool     sp_dirty;
    int      pending_sp;
    uint32_t sp_dirty_ms;

    lv_obj_t *name_lbl;
    lv_obj_t *inside_lbl;
    lv_obj_t *fan_lbl;
    lv_obj_t *state_lbl;
    lv_obj_t *mode_btn;
    lv_obj_t *mode_lbl;
    lv_obj_t *minus_btn;
    lv_obj_t *sp_lbl;
    lv_obj_t *plus_btn;
} thermo_zone_t;

typedef struct {
    thermo_ctx_t *ctx;
    uint8_t       mode;
} mode_opt_t;

struct thermo_ctx {
    bool          valid;
    thermo_zone_t zones[UI_THERMOSTAT_ZONES];

    lv_timer_t   *sp_timer;             /* setpoint-send debounce, one-shot */

    /*
     * The mode picker is built ONCE and toggled with the HIDDEN flag, never
     * created/deleted per open. Rebuilding a full-screen lv_layer_top() tree
     * on every tap made the RGB panel flicker, and deleting it from a child
     * button's own callback (even async) fell through to the screen
     * underneath and navigated away. A persistent, hidden-when-idle overlay
     * behaves exactly like the idle-dim overlay, which has always been fine.
     */
    lv_obj_t     *popup;               /* full-screen backdrop, HIDDEN when idle */
    lv_obj_t     *popup_title;
    lv_obj_t     *popup_btn[N_MODES];
    lv_obj_t     *popup_btn_lbl[N_MODES];
    mode_opt_t    popup_opt[N_MODES];  /* {ctx, mode} per button, set once */
    uint8_t       popup_zone;          /* which zone the picker currently edits */
};

static ui_thermostat_cmd_cb_t s_cmd_cb;

void ui_thermostat_set_cmd_cb(ui_thermostat_cmd_cb_t cb)
{
    s_cmd_cb = cb;
}

/* ------------------------------------------------------------ helpers --- */

static sp_field_t active_sp_field(uint8_t mode)
{
    switch (mode) {
    case EASYTOUCH_MODE_COOL:     return SP_COOL;
    case EASYTOUCH_MODE_HEAT:     return SP_HEAT;
    case EASYTOUCH_MODE_AQUA_HOT: return SP_HEAT;
    default:                      return SP_NONE;
    }
}

static uint8_t fan_for_mode(const easytouch_zone_t *z, uint8_t mode)
{
    switch (mode) {
    case EASYTOUCH_MODE_COOL:     return z->cool_fan;
    case EASYTOUCH_MODE_HEAT:
    case EASYTOUCH_MODE_AQUA_HOT: return z->heat_fan;
    case EASYTOUCH_MODE_AUTO:     return z->auto_fan;
    case EASYTOUCH_MODE_FAN:      return z->fan_only;
    default:                      return EASYTOUCH_FAN_OFF;
    }
}

static uint8_t status_sp(const easytouch_zone_t *z, sp_field_t f)
{
    if (f == SP_COOL) return z->cool_sp;
    if (f == SP_HEAT) return z->heat_sp;
    return 0;
}

static uint8_t effective_mode(const thermo_zone_t *z)
{
    return z->mode_dirty ? z->pending_mode : z->last.mode;
}

static void send_change(const easytouch_change_t *c)
{
    if (s_cmd_cb != NULL) {
        s_cmd_cb(c);
    }
}

static void set_ctl_enabled(thermo_zone_t *z, bool mode_en, bool sp_en)
{
    if (mode_en) {
        lv_obj_remove_state(z->mode_btn, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(z->mode_btn, LV_STATE_DISABLED);
    }
    for (int i = 0; i < 2; i++) {
        lv_obj_t *b = i == 0 ? z->minus_btn : z->plus_btn;
        if (sp_en) {
            lv_obj_remove_state(b, LV_STATE_DISABLED);
        } else {
            lv_obj_add_state(b, LV_STATE_DISABLED);
        }
    }
}

/* Repaint one zone from its cached last status plus any pending overrides.
 * Pending values are tinted amber ("sending, not yet confirmed"). */
static void paint_zone(thermo_zone_t *z)
{
    char buf[16];

    if (!z->owner->valid || !z->last_valid || !z->last.present) {
        lv_label_set_text(z->inside_lbl, "--");
        lv_label_set_text(z->mode_lbl, z->owner->valid ? "OFF" : "--");
        lv_obj_set_style_text_color(z->mode_lbl, UI_COLOR_TEXT_ON_LIT, 0);
        lv_label_set_text(z->fan_lbl, "");
        lv_label_set_text(z->state_lbl, "");
        lv_label_set_text(z->sp_lbl, "--");
        lv_obj_set_style_text_color(z->sp_lbl, UI_COLOR_TEXT, 0);
        set_ctl_enabled(z, false, false);
        return;
    }

    const easytouch_zone_t *s = &z->last;
    const uint8_t em = effective_mode(z);
    const sp_field_t f = active_sp_field(em);

    snprintf(buf, sizeof(buf), "%dF", (int)s->inside_f);
    lv_label_set_text(z->inside_lbl, buf);

    lv_label_set_text(z->mode_lbl, easytouch_mode_str(em));
    lv_obj_set_style_text_color(z->mode_lbl,
                                z->mode_dirty ? UI_COLOR_AMBER : UI_COLOR_TEXT_ON_LIT, 0);

    if (em == EASYTOUCH_MODE_OFF) {
        lv_label_set_text(z->fan_lbl, "");
    } else {
        snprintf(buf, sizeof(buf), "Fan %s", easytouch_fan_str(fan_for_mode(s, em)));
        lv_label_set_text(z->fan_lbl, buf);
    }

    if (s->current_mode == EASYTOUCH_CURRENT_COOLING) {
        lv_label_set_text(z->state_lbl, "cooling");
        lv_obj_set_style_text_color(z->state_lbl, UI_COLOR_CARD_ON, 0);
    } else if (s->current_mode == EASYTOUCH_CURRENT_HEATING) {
        lv_label_set_text(z->state_lbl, "heating");
        lv_obj_set_style_text_color(z->state_lbl, UI_COLOR_WARN, 0);
    } else {
        lv_label_set_text(z->state_lbl, "");
    }

    if (f == SP_NONE) {
        lv_label_set_text(z->sp_lbl, "--");
        lv_obj_set_style_text_color(z->sp_lbl, UI_COLOR_TEXT, 0);
    } else {
        const int shown = z->sp_dirty ? z->pending_sp : (int)status_sp(s, f);
        snprintf(buf, sizeof(buf), "%dF", shown);
        lv_label_set_text(z->sp_lbl, buf);
        lv_obj_set_style_text_color(z->sp_lbl,
                                    z->sp_dirty ? UI_COLOR_AMBER : UI_COLOR_TEXT, 0);
    }

    set_ctl_enabled(z, true, f != SP_NONE);
}

/* ---------------------------------------------------- setpoint -/+ ----- */

static void sp_timer_cb(lv_timer_t *t)
{
    thermo_ctx_t *ctx = lv_timer_get_user_data(t);
    ctx->sp_timer = NULL;   /* one-shot; LVGL deletes it after this run */

    for (uint8_t i = 0; i < UI_THERMOSTAT_ZONES; i++) {
        thermo_zone_t *z = &ctx->zones[i];
        if (!z->sp_dirty) {
            continue;
        }
        const sp_field_t f = active_sp_field(effective_mode(z));
        if (f == SP_NONE) {
            continue;
        }
        easytouch_change_t c = { .zone = z->index };
        if (f == SP_COOL) {
            c.set_cool_sp = true;
            c.cool_sp = (uint8_t)z->pending_sp;
        } else {
            c.set_heat_sp = true;
            c.heat_sp = (uint8_t)z->pending_sp;
        }
        send_change(&c);
    }
}

static void arm_sp_timer(thermo_ctx_t *ctx)
{
    if (ctx->sp_timer != NULL) {
        lv_timer_reset(ctx->sp_timer);   /* restart the countdown */
        return;
    }
    ctx->sp_timer = lv_timer_create(sp_timer_cb, SP_DEBOUNCE_MS, ctx);
    lv_timer_set_repeat_count(ctx->sp_timer, 1);
}

static void bump_local(thermo_zone_t *z, int delta)
{
    if (!z->owner->valid || !z->last_valid || !z->last.present) {
        return;
    }
    const sp_field_t f = active_sp_field(effective_mode(z));
    if (f == SP_NONE) {
        return;
    }
    if (!z->sp_dirty) {
        z->pending_sp = (int)status_sp(&z->last, f);
        z->sp_dirty = true;
    }
    z->pending_sp += delta;
    if (z->pending_sp < SP_MIN_F) z->pending_sp = SP_MIN_F;
    if (z->pending_sp > SP_MAX_F) z->pending_sp = SP_MAX_F;
    z->sp_dirty_ms = lv_tick_get();

    paint_zone(z);
    arm_sp_timer(z->owner);
}

static void minus_cb(lv_event_t *e) { bump_local(lv_event_get_user_data(e), -1); }
static void plus_cb(lv_event_t *e)  { bump_local(lv_event_get_user_data(e), +1); }

/* ---------------------------------------------------- mode picker ------ */

static void popup_hide(thermo_ctx_t *ctx)
{
    if (ctx->popup != NULL) {
        lv_obj_add_flag(ctx->popup, LV_OBJ_FLAG_HIDDEN);
    }
}

static void popup_backdrop_cb(lv_event_t *e)
{
    /* Tap on the scrim (outside the card) = cancel. Child buttons consume
     * their own clicks, so this only fires for a miss. */
    popup_hide(lv_event_get_user_data(e));
}

static void mode_pick_cb(lv_event_t *e)
{
    mode_opt_t *opt = lv_event_get_user_data(e);
    thermo_ctx_t *ctx = opt->ctx;
    thermo_zone_t *z = &ctx->zones[ctx->popup_zone];

    z->pending_mode = opt->mode;
    z->mode_dirty = true;
    z->mode_dirty_ms = lv_tick_get();
    /* A mode change can move which setpoint field is active; drop a stale
     * pending setpoint so the card seeds fresh from status next bump. */
    z->sp_dirty = false;

    easytouch_change_t c = { .zone = z->index, .set_mode = true, .mode = opt->mode };
    send_change(&c);

    paint_zone(z);
    popup_hide(ctx);
}

/* Build the picker once. It lives hidden on lv_layer_top() and is toggled
 * by open_mode_popup() / popup_hide(); never created or deleted per tap. */
static void build_mode_popup(thermo_ctx_t *ctx)
{
    lv_obj_t *backdrop = lv_obj_create(lv_layer_top());
    lv_obj_set_size(backdrop, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(backdrop, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(backdrop, LV_OPA_60, 0);
    lv_obj_set_style_border_width(backdrop, 0, 0);
    lv_obj_set_style_radius(backdrop, 0, 0);
    lv_obj_set_style_pad_all(backdrop, 0, 0);
    lv_obj_remove_flag(backdrop, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(backdrop, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(backdrop, popup_backdrop_cb, LV_EVENT_CLICKED, ctx);
    ctx->popup = backdrop;

    lv_obj_t *card = lv_obj_create(backdrop);
    lv_obj_set_style_bg_color(card, UI_COLOR_CARD, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(card, 2, 0);
    lv_obj_set_style_border_color(card, UI_COLOR_OFF, 0);
    lv_obj_set_style_radius(card, UI_CARD_RADIUS, 0);
    lv_obj_set_style_pad_all(card, 14, 0);
    lv_obj_set_style_pad_row(card, 8, 0);
    lv_obj_set_size(card, 320, LV_SIZE_CONTENT);
    lv_obj_center(card);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    ctx->popup_title = lv_label_create(card);
    lv_label_set_text(ctx->popup_title, "MODE");
    lv_obj_set_style_text_font(ctx->popup_title, UI_FONT_READOUT_CAPTION, 0);
    lv_obj_set_style_text_color(ctx->popup_title, UI_COLOR_TEXT_DIM, 0);

    for (uint8_t i = 0; i < N_MODES; i++) {
        ctx->popup_opt[i] = (mode_opt_t){ ctx, k_modes[i] };

        lv_obj_t *b = lv_button_create(card);
        lv_obj_add_style(b, &ui_style_card, LV_STATE_DEFAULT);
        lv_obj_add_style(b, &ui_style_card_pressed, LV_STATE_PRESSED);
        lv_obj_set_size(b, LV_PCT(100), 46);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(b, UI_CARD_RADIUS, 0);
        lv_obj_add_event_cb(b, mode_pick_cb, LV_EVENT_CLICKED, &ctx->popup_opt[i]);
        ctx->popup_btn[i] = b;

        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, easytouch_mode_str(k_modes[i]));
        lv_obj_set_style_text_font(l, UI_FONT_BTN, 0);
        lv_obj_center(l);
        ctx->popup_btn_lbl[i] = l;
    }

    lv_obj_t *hint = lv_label_create(card);
    lv_label_set_text(hint, "tap outside to cancel");
    lv_obj_set_style_text_font(hint, UI_FONT_READOUT_CAPTION, 0);
    lv_obj_set_style_text_color(hint, UI_COLOR_TEXT_DIM, 0);
}

static void open_mode_popup(thermo_zone_t *z)
{
    thermo_ctx_t *ctx = z->owner;
    if (!ctx->valid || !z->last_valid || !z->last.present) {
        return;
    }
    if (ctx->popup == NULL) {
        build_mode_popup(ctx);
    }
    ctx->popup_zone = z->index;

    char buf[24];
    snprintf(buf, sizeof(buf), "%s  MODE", lv_label_get_text(z->name_lbl));
    lv_label_set_text(ctx->popup_title, buf);

    const uint8_t cur = effective_mode(z);
    for (uint8_t i = 0; i < N_MODES; i++) {
        const bool sel = (k_modes[i] == cur);
        lv_obj_set_style_bg_color(ctx->popup_btn[i],
                                  sel ? UI_COLOR_CARD_ON : UI_COLOR_OFF, 0);
        lv_obj_set_style_text_color(ctx->popup_btn_lbl[i],
                                    sel ? UI_COLOR_TEXT_ON_LIT : UI_COLOR_TEXT, 0);
    }

    lv_obj_remove_flag(ctx->popup, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(ctx->popup);
}

static void mode_btn_cb(lv_event_t *e)
{
    open_mode_popup(lv_event_get_user_data(e));
}

/* ------------------------------------------------------------ build ---- */

static void style_control(lv_obj_t *b, lv_obj_t *label)
{
    lv_obj_set_style_bg_color(b, UI_COLOR_CARD_ON, LV_PART_MAIN);
    lv_obj_set_style_bg_color(b, UI_COLOR_CARD, LV_PART_MAIN | LV_STATE_DISABLED);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(b, UI_CARD_RADIUS, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, UI_COLOR_TEXT_ON_LIT, 0);
    lv_obj_set_style_text_color(label, UI_COLOR_TEXT_DIM, LV_STATE_DISABLED);
}

static lv_obj_t *mini_btn(lv_obj_t *parent, const char *txt, lv_event_cb_t cb,
                          void *user_data)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_add_style(b, &ui_style_card, LV_STATE_DEFAULT);
    lv_obj_add_style(b, &ui_style_card_pressed, LV_STATE_PRESSED);
    lv_obj_set_size(b, 56, 48);
    lv_obj_set_style_pad_all(b, 0, 0);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, UI_FONT_BATT_VALUE, 0);
    lv_obj_center(l);
    style_control(b, l);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user_data);
    return b;
}

static lv_obj_t *plain_row(lv_obj_t *parent)
{
    lv_obj_t *r = lv_obj_create(parent);
    lv_obj_set_style_bg_opa(r, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(r, 0, 0);
    lv_obj_set_style_pad_all(r, 0, 0);
    lv_obj_set_size(r, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    return r;
}

static void build_zone(lv_obj_t *parent, thermo_zone_t *z)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_add_style(card, &ui_style_card, 0);
    lv_obj_set_width(card, LV_PCT(100));
    lv_obj_set_flex_grow(card, 1);
    lv_obj_set_style_pad_all(card, UI_CARD_PAD, 0);
    lv_obj_set_style_pad_row(card, 4, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    lv_obj_t *head = plain_row(card);
    z->name_lbl = lv_label_create(head);
    lv_label_set_text(z->name_lbl, "ZONE");
    lv_obj_set_style_text_font(z->name_lbl, UI_FONT_BTN, 0);
    lv_obj_set_style_text_color(z->name_lbl, UI_COLOR_TEXT, 0);

    z->inside_lbl = lv_label_create(head);
    lv_label_set_text(z->inside_lbl, "--");
    lv_obj_set_style_text_font(z->inside_lbl, UI_FONT_BATT_VALUE, 0);
    lv_obj_set_style_text_color(z->inside_lbl, UI_COLOR_TEXT, 0);

    lv_obj_t *sub = plain_row(card);
    z->fan_lbl = lv_label_create(sub);
    lv_label_set_text(z->fan_lbl, "");
    lv_obj_set_style_text_font(z->fan_lbl, UI_FONT_READOUT_CAPTION, 0);
    lv_obj_set_style_text_color(z->fan_lbl, UI_COLOR_TEXT_DIM, 0);

    z->state_lbl = lv_label_create(sub);
    lv_label_set_text(z->state_lbl, "");
    lv_obj_set_style_text_font(z->state_lbl, UI_FONT_READOUT_CAPTION, 0);
    lv_obj_set_style_text_color(z->state_lbl, UI_COLOR_TEXT_DIM, 0);

    lv_obj_t *ctl = plain_row(card);
    lv_obj_set_style_pad_column(ctl, 6, 0);

    z->mode_btn = lv_button_create(ctl);
    lv_obj_add_style(z->mode_btn, &ui_style_card, LV_STATE_DEFAULT);
    lv_obj_add_style(z->mode_btn, &ui_style_card_pressed, LV_STATE_PRESSED);
    lv_obj_set_size(z->mode_btn, 118, 48);
    lv_obj_set_style_pad_all(z->mode_btn, 0, 0);
    lv_obj_add_event_cb(z->mode_btn, mode_btn_cb, LV_EVENT_CLICKED, z);
    z->mode_lbl = lv_label_create(z->mode_btn);
    lv_label_set_text(z->mode_lbl, "--");
    lv_obj_set_style_text_font(z->mode_lbl, UI_FONT_BTN, 0);
    lv_obj_center(z->mode_lbl);
    style_control(z->mode_btn, z->mode_lbl);

    z->minus_btn = mini_btn(ctl, "-", minus_cb, z);

    z->sp_lbl = lv_label_create(ctl);
    lv_label_set_text(z->sp_lbl, "--");
    lv_obj_set_style_text_font(z->sp_lbl, UI_FONT_BATT_VALUE, 0);
    lv_obj_set_style_text_color(z->sp_lbl, UI_COLOR_TEXT, 0);

    z->plus_btn = mini_btn(ctl, "+", plus_cb, z);
}

static void ctx_delete_cb(lv_event_t *e)
{
    thermo_ctx_t *ctx = lv_event_get_user_data(e);
    if (ctx->sp_timer != NULL) {
        lv_timer_delete(ctx->sp_timer);
    }
    if (ctx->popup != NULL) {
        lv_obj_delete(ctx->popup);   /* it lives on lv_layer_top(), not our tree */
    }
    lv_free(ctx);
}

lv_obj_t *ui_thermostat_create(lv_obj_t *parent)
{
    thermo_ctx_t *ctx = lv_malloc(sizeof(thermo_ctx_t));
    LV_ASSERT_MALLOC(ctx);
    memset(ctx, 0, sizeof(*ctx));

    lv_obj_t *wrapper = lv_obj_create(parent);
    lv_obj_set_style_bg_opa(wrapper, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(wrapper, 0, 0);
    lv_obj_set_style_pad_all(wrapper, 0, 0);
    lv_obj_set_style_pad_row(wrapper, 6, 0);
    lv_obj_set_size(wrapper, LV_PCT(100), LV_PCT(100));
    lv_obj_remove_flag(wrapper, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(wrapper, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(wrapper, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_user_data(wrapper, ctx);
    lv_obj_add_event_cb(wrapper, ctx_delete_cb, LV_EVENT_DELETE, ctx);

    for (uint8_t z = 0; z < UI_THERMOSTAT_ZONES; z++) {
        ctx->zones[z].owner = ctx;
        ctx->zones[z].index = z;
        build_zone(wrapper, &ctx->zones[z]);
        char def[12];
        snprintf(def, sizeof(def), "ZONE %u", (unsigned)z);
        lv_label_set_text(ctx->zones[z].name_lbl, def);
    }
    return wrapper;
}

void ui_thermostat_set_zone_names(lv_obj_t *w, const char *const names[UI_THERMOSTAT_ZONES])
{
    thermo_ctx_t *ctx = lv_obj_get_user_data(w);
    if (ctx == NULL || names == NULL) {
        return;
    }
    for (uint8_t z = 0; z < UI_THERMOSTAT_ZONES; z++) {
        if (names[z] != NULL) {
            lv_label_set_text(ctx->zones[z].name_lbl, names[z]);
        }
    }
}

void ui_thermostat_set(lv_obj_t *w, const easytouch_status_t *st, bool valid)
{
    thermo_ctx_t *ctx = lv_obj_get_user_data(w);
    if (ctx == NULL) {
        return;
    }
    ctx->valid = valid && st != NULL;

    for (uint8_t zi = 0; zi < UI_THERMOSTAT_ZONES; zi++) {
        thermo_zone_t *z = &ctx->zones[zi];

        if (ctx->valid && zi < EASYTOUCH_MAX_ZONES) {
            z->last = st->zones[zi];
            z->last_valid = true;
        } else {
            z->last_valid = false;
        }

        /* Reconcile optimistic overrides against the fresh status. */
        const uint32_t now = lv_tick_get();
        if (z->mode_dirty) {
            if ((z->last_valid && z->last.present && z->last.mode == z->pending_mode) ||
                (now - z->mode_dirty_ms) > PENDING_STALE_MS) {
                z->mode_dirty = false;
            }
        }
        if (z->sp_dirty) {
            const sp_field_t f = active_sp_field(effective_mode(z));
            const bool confirmed = z->last_valid && z->last.present && f != SP_NONE &&
                                   (int)status_sp(&z->last, f) == z->pending_sp;
            if (confirmed || (now - z->sp_dirty_ms) > PENDING_STALE_MS) {
                z->sp_dirty = false;
            }
        }

        paint_zone(z);
    }
}

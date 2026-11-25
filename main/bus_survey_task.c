/*
 * bus_survey_task — RV-C bus survey + probe (diagnostic build, issue #96).
 *
 * Why it is shaped like this:
 *   - twai_rx_task calls bus_survey_on_frame() and nothing more. A log line at
 *     115200 baud takes ~9 ms, and the CAN rx queue is 32 deep, so ANY printing
 *     from the rx task drops frames. New-key and special events go through a
 *     small queue to a low-priority reporter task instead.
 *   - Console input comes from stdin (the UART console) and from the USB-Serial/
 *     JTAG RX FIFO, polled directly because the IDF secondary console is
 *     output-only. The stop command is handled in its own small task, so a long
 *     table dump in the reporter can never delay it.
 *   - The probe is idle at boot and refuses to start unless the survey has run
 *     long enough and none of its DGNs has ever been seen on the bus.
 *
 * Everything is compiled out unless CONFIG_FIREFLY_BUS_SURVEY is set.
 */
#include "bus_survey_task.h"

#if CONFIG_FIREFLY_BUS_SURVEY

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "soc/soc_caps.h"

/* The USB-Serial/JTAG console is output-only through the VFS (the secondary console
 * never reads), but the peripheral's RX FIFO can be polled directly -- the same call the
 * VFS itself uses when USB is the PRIMARY console. That lets a single USB cable carry
 * both the log and the commands. No driver is installed, so nothing here touches TX. */
#if SOC_USB_SERIAL_JTAG_SUPPORTED && \
    (CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG || CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG)
#include "hal/usb_serial_jtag_ll.h"
#define SURVEY_USB_RX 1
#endif

#include "bus_survey.h"
#include "panel_config.h"
#include "rvc_protocol.h"

static const char *TAG = "survey";

#define CAP             CONFIG_FIREFLY_BUS_SURVEY_ENTRIES
#define REPORT_MS       (CONFIG_FIREFLY_BUS_SURVEY_REPORT_S * 1000u)
#define FULL_REPORT_MS  (CONFIG_FIREFLY_BUS_SURVEY_TABLE_S * 1000u)
#define EV_QUEUE_LEN    48
#define CMD_QUEUE_LEN   8
#define DELTA_PCT       30u
#define DELTA_MIN_COUNT 10u

typedef enum { EV_NEW = 1, EV_CLAIM, EV_REQUEST, EV_DMRV, EV_STOP } ev_kind_t;

typedef struct {
    uint8_t  kind;
    uint8_t  sa;
    uint8_t  dlc;
    uint8_t  da;
    uint32_t dgn;
    uint32_t t_ms;
    uint8_t  d[8];
} ev_t;

static survey_t         s_sv;
static survey_entry_t  *s_tab;
static QueueHandle_t    s_ev_q;
static QueueHandle_t    s_cmd_q;

static volatile bool     s_clear_req;
static volatile uint32_t s_std_frames;
static volatile uint32_t s_bits_total;
static volatile uint32_t s_ev_dropped;
static volatile uint8_t  s_probe_stage;   /* 0 = idle */

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void hex8(char *out, const uint8_t *d, uint8_t n)
{
    for (uint8_t i = 0; i < n; i++) {
        snprintf(out + 2u * i, 3, "%02X", d[i]);
    }
    if (n == 0) {
        out[0] = '\0';
    }
}

/* ---------------------------------------------------------------- probe -- */

#if CONFIG_FIREFLY_BUS_PROBE

#define PROBE_DGN_COUNT 16u
#define PROBE_BASE      ((uint32_t)CONFIG_FIREFLY_PROBE_DGN_BASE)

/* Stage 2 runs a 10 s cycle of 100 ticks (100 ms each), holding at most one frame
 * per tick. Streams are placed into it once, at start-up. */
#define SCHED_TICKS     100u
#define SCHED_EMPTY     0xFFu

/* Instance for command-type DGNs: "not available", so nothing that later learns to
 * act on these DGNs could ever take a probe frame for a real command. */
#define PROBE_INST_NA   0xFFu

typedef enum {
    STOP_OPERATOR = 1, STOP_COLLISION, STOP_DMRV, STOP_TWAI_STATE,
    STOP_TX_FAIL, STOP_BUS_ERR, STOP_TX_STUCK,
} stop_reason_t;

static const char *const k_stop_text[] = {
    "none", "operator", "probe DGN seen from another source", "new DM_RV on the bus",
    "TWAI not running", "TX failures (nobody ACKing?)", "bus errors", "TX queue stuck",
};

/* Offsets match docs/RVC-CUSTOM-DGNS.md (v2 map). */
static const char *const k_probe_name[PROBE_DGN_COUNT] = {
    "hvac_status", "hvac_extras", "bat_pack1", "bat_pack2", "bat_pack3",
    "bank_a", "shore_line", "solar_a", "pack_detail", "bank_b",
    "hvac_cmd", "solar_b", "shore_pedestal", "valve_status", "valve_cmd",
    "gw_heartbeat",
};

/* One planned stream: DGN offset, instance, period in ticks, first tick. Rates are
 * the ones the spec plans (sec. 5), so stage 2 looks like the real traffic: ~5 fps. */
typedef struct {
    uint8_t off;
    uint8_t inst;
    uint8_t period;
    uint8_t phase;
} probe_stream_t;

static const probe_stream_t k_streams[] = {
    { 0x0, 0, 20, 0 }, { 0x0, 1, 20, 1 }, { 0x0, 2, 20, 2 },        /* HVAC status, 2 s per zone */
    { 0x1, 0, 100, 3 }, { 0x1, 1, 100, 4 }, { 0x1, 2, 100, 5 },     /* HVAC extras, 10 s */
    { 0x2, 1, 50, 6 }, { 0x3, 2, 50, 7 }, { 0x4, 3, 50, 8 },        /* packs 1..3, 5 s */
    { 0x5, 1, 50, 9 }, { 0x9, 1, 50, 10 },                          /* bank A / B, 5 s */
    { 0x8, 1, 100, 11 }, { 0x8, 2, 100, 12 }, { 0x8, 3, 100, 13 },  /* pack detail, 10 s */
    { 0x6, 0, 20, 14 }, { 0x6, 1, 20, 15 },                         /* shore line A / B, 2 s */
    { 0xC, 0, 100, 16 },                                            /* shore pedestal, 10 s */
    { 0x7, 1, 50, 17 }, { 0xB, 1, 50, 18 },                         /* solar A / B, 5 s */
    { 0xF, 1, 50, 19 },                                             /* gateway heartbeat, 5 s */
    { 0xD, 0, 100, 21 }, { 0xD, 1, 100, 22 },                       /* valve status, 10 s here */
    { 0xA, PROBE_INST_NA, 100, 23 },                                /* HVAC command, 10 s */
    { 0xE, PROBE_INST_NA, 100, 24 },                                /* valve command, 10 s */
};

static uint8_t s_sched_off[SCHED_TICKS];
static uint8_t s_sched_inst[SCHED_TICKS];

/* Greedy placement: each stream's frames go on their planned ticks, sliding to the
 * next free tick on a clash, so the cycle never sends two frames in one tick. */
static void probe_build_schedule(void)
{
    memset(s_sched_off, SCHED_EMPTY, sizeof(s_sched_off));
    for (size_t s = 0; s < sizeof(k_streams) / sizeof(k_streams[0]); s++) {
        const probe_stream_t *st = &k_streams[s];
        for (uint32_t t = st->phase; t < SCHED_TICKS; t += st->period) {
            uint32_t k = t;
            for (uint32_t tries = 0; tries < SCHED_TICKS && s_sched_off[k] != SCHED_EMPTY; tries++) {
                k = (k + 1u) % SCHED_TICKS;
            }
            if (s_sched_off[k] == SCHED_EMPTY) {
                s_sched_off[k] = st->off;
                s_sched_inst[k] = st->inst;
            }
        }
    }
}

static uint32_t s_probe_tx_ok, s_probe_tx_fail;
static uint32_t s_probe_txfail0, s_probe_buserr0;
static uint32_t s_probe_start_ms;

static void post_ev(const ev_t *ev)
{
    if (xQueueSend(s_ev_q, ev, 0) != pdTRUE) {
        s_ev_dropped++;
    }
}

/* Safe from any task, including twai_rx_task: no logging, just flags + an event. */
static void probe_stop(stop_reason_t why)
{
    if (s_probe_stage == 0) {
        return;
    }
    s_probe_stage = 0;
    ev_t ev = { .kind = EV_STOP, .sa = (uint8_t)why, .t_ms = now_ms() };
    post_ev(&ev);
}

static void probe_send(uint8_t dgn_off, uint8_t instance, uint8_t ctr, uint8_t stage)
{
    twai_message_t m = {
        .identifier = rvc_id_pack(RVC_PRIORITY_DEFAULT, PROBE_BASE + dgn_off, PANEL_SOURCE_ADDR),
        .extd = 1,
        .data_length_code = 8,
    };
    /* Obviously synthetic: instance, counter, ASCII "FT", stage, then N/A (0xFF). */
    const uint8_t d[8] = { instance, ctr, 0x46, 0x54, stage, 0xFF, 0xFF, 0xFF };
    memcpy(m.data, d, sizeof(d));
    if (twai_transmit(&m, 0) == ESP_OK) {
        s_probe_tx_ok++;
    } else {
        s_probe_tx_fail++;
    }
}

static void probe_safety_check(void)
{
    twai_status_info_t st;
    if (twai_get_status_info(&st) != ESP_OK) {
        return;
    }
    if (st.state != TWAI_STATE_RUNNING) {
        probe_stop(STOP_TWAI_STATE);
    } else if (st.tx_failed_count - s_probe_txfail0 >= 3) {
        probe_stop(STOP_TX_FAIL);
    } else if (st.bus_error_count - s_probe_buserr0 >= 10) {
        probe_stop(STOP_BUS_ERR);
    } else if (st.msgs_to_tx > 4) {
        probe_stop(STOP_TX_STUCK);
    }
}

static void probe_task(void *arg)
{
    (void)arg;
    uint32_t tick = 0;
    uint8_t ctr = 0;
    TickType_t wake = xTaskGetTickCount();

    for (;;) {
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(100));
        const uint8_t stage = s_probe_stage;
        if (stage == 0) {
            tick = 0;
            continue;
        }
        tick++;

        if (stage == 1) {
            if (tick % 10u == 0) {
                probe_send(0, 0x01, ctr++, 1);
            }
        } else {
            /* Stage 2: all 16 DGNs at their planned rates from the 10 s schedule,
             * at most one frame per 100 ms tick (well under the 20 fps cap). */
            const uint32_t slot = tick % SCHED_TICKS;
            if (s_sched_off[slot] != SCHED_EMPTY) {
                probe_send(s_sched_off[slot], s_sched_inst[slot], ctr, 2);
            }
            if (slot == SCHED_TICKS - 1u) {
                ctr++;
            }
        }

        if (tick % 10u == 0) {
            probe_safety_check();
        }
    }
}

static void probe_start(uint8_t stage)
{
    const uint32_t elapsed_s = (now_ms() - s_sv.start_ms) / 1000u;
    if (elapsed_s < (uint32_t)CONFIG_FIREFLY_PROBE_MIN_SURVEY_S) {
        ESP_LOGW(TAG, "PROBE REFUSED: survey has run %" PRIu32 " s, need %d s",
                 elapsed_s, CONFIG_FIREFLY_PROBE_MIN_SURVEY_S);
        return;
    }

    bool clash = false;
    for (uint32_t i = 0; i < PROBE_DGN_COUNT; i++) {
        if (survey_dgn_seen(&s_sv, PROBE_BASE + i)) {
            ESP_LOGW(TAG, "PROBE REFUSED: DGN %05" PRIX32 " (%s) is already in use on this bus",
                     PROBE_BASE + i, k_probe_name[i]);
            clash = true;
        }
    }
    if (clash) {
        return;
    }

    twai_status_info_t st;
    if (twai_get_status_info(&st) != ESP_OK || st.state != TWAI_STATE_RUNNING) {
        ESP_LOGW(TAG, "PROBE REFUSED: TWAI is not running");
        return;
    }
    if (!s_sv.have_baseline) {
        survey_set_baseline(&s_sv, now_ms());
        ESP_LOGI(TAG, "BASELINE set automatically at probe start");
    }

    s_probe_txfail0 = st.tx_failed_count;
    s_probe_buserr0 = st.bus_error_count;
    s_probe_tx_ok = 0;
    s_probe_tx_fail = 0;
    s_probe_start_ms = now_ms();
    s_probe_stage = stage;
    ESP_LOGW(TAG, "PROBE STAGE %u STARTED: DGN block %05" PRIX32 "..%05" PRIX32
             ", source 0x%02X, x = stop",
             stage, PROBE_BASE, PROBE_BASE + PROBE_DGN_COUNT - 1u, PANEL_SOURCE_ADDR);
}

static void probe_report(void)
{
    ESP_LOGI(TAG, "PROBE stage=%u tx_ok=%" PRIu32 " tx_fail=%" PRIu32 " running_s=%" PRIu32
             " block=%05" PRIX32 "..%05" PRIX32 " min_survey_s=%d",
             s_probe_stage, s_probe_tx_ok, s_probe_tx_fail,
             s_probe_stage ? (now_ms() - s_probe_start_ms) / 1000u : 0u,
             PROBE_BASE, PROBE_BASE + PROBE_DGN_COUNT - 1u, CONFIG_FIREFLY_PROBE_MIN_SURVEY_S);
    for (uint32_t i = 0; i < PROBE_DGN_COUNT; i++) {
        ESP_LOGI(TAG, "  %05" PRIX32 " %-14s %s", PROBE_BASE + i, k_probe_name[i],
                 survey_dgn_seen(&s_sv, PROBE_BASE + i) ? "SEEN ON BUS" : "free");
    }
}

#endif /* CONFIG_FIREFLY_BUS_PROBE */

/* ---------------------------------------------------------------- rx hook -- */

void bus_survey_on_frame(const twai_message_t *msg)
{
    if (s_tab == NULL || msg->rtr) {
        return;
    }
    if (s_clear_req) {
        s_clear_req = false;
        survey_init(&s_sv, s_tab, CAP, now_ms());
        s_bits_total = 0;
        s_std_frames = 0;
    }
    if (!msg->extd) {
        s_std_frames++;
        return;
    }

    const uint32_t t = now_ms();
    const uint32_t dgn17 = (msg->identifier >> 8) & 0x1FFFFu;
    const uint8_t sa = (uint8_t)(msg->identifier & 0xFFu);
    const uint8_t dlc = msg->data_length_code;

    const survey_result_t r = survey_note(&s_sv, t, dgn17, sa, dlc, msg->data);
    s_bits_total += survey_frame_bits(dlc);

    uint8_t da;
    const uint32_t dgn = survey_canonical_dgn(dgn17, &da);

    ev_t ev = { .sa = sa, .dlc = dlc, .da = da, .dgn = dgn, .t_ms = t };
    memcpy(ev.d, msg->data, 8);
    bool post = false;

    if (r == SURVEY_NEW) {
        ev.kind = EV_NEW;
        post = true;
    }
    /* Special groups report every time the payload changes, not just first sight. */
    if (dgn == SURVEY_DGN_ADDR_CLAIM && (r == SURVEY_NEW || r == SURVEY_CHANGED)) {
        ev.kind = EV_CLAIM;
        post = true;
    } else if (dgn == SURVEY_DGN_REQUEST && (r == SURVEY_NEW || r == SURVEY_CHANGED)) {
        ev.kind = EV_REQUEST;
        post = true;
    } else if ((dgn == SURVEY_DGN_DM_RV || dgn == SURVEY_DGN_DM1) &&
               (r == SURVEY_NEW || r == SURVEY_CHANGED)) {
        ev.kind = EV_DMRV;   /* NEW keys are reported once as NEW too; changes are the interesting part */
        post = true;
    }
    if (post && xQueueSend(s_ev_q, &ev, 0) != pdTRUE) {
        s_ev_dropped++;
    }

#if CONFIG_FIREFLY_BUS_PROBE
    if (s_probe_stage != 0) {
        if (dgn >= PROBE_BASE && dgn < PROBE_BASE + PROBE_DGN_COUNT && sa != PANEL_SOURCE_ADDR) {
            probe_stop(STOP_COLLISION);
        } else if ((dgn == SURVEY_DGN_DM_RV || dgn == SURVEY_DGN_DM1) &&
                   (r == SURVEY_NEW || r == SURVEY_CHANGED)) {
            /* Any device reporting a new or changed diagnostic while we are probing is
             * treated as our fault until proven otherwise. */
            probe_stop(STOP_DMRV);
        }
    }
#endif
}

/* --------------------------------------------------------------- reporter -- */

static void print_event(const ev_t *ev)
{
    char hx[17];
    hex8(hx, ev->d, ev->dlc > 8 ? 8 : ev->dlc);
    switch (ev->kind) {
    case EV_NEW:
        ESP_LOGI(TAG, "NEW %05" PRIX32 " sa=%02X b0=%02X da=%02X dlc=%u d=%s",
                 ev->dgn, ev->sa, ev->dlc ? ev->d[0] : 0xFF, ev->da, ev->dlc, hx);
        break;
    case EV_CLAIM:
        ESP_LOGI(TAG, "CLAIM sa=%02X name=%s", ev->sa, hx);
        break;
    case EV_REQUEST:
        ESP_LOGI(TAG, "REQUEST from sa=%02X to da=%02X d=%s", ev->sa, ev->da, hx);
        break;
    case EV_DMRV:
        ESP_LOGW(TAG, "DIAG %05" PRIX32 " from sa=%02X d=%s", ev->dgn, ev->sa, hx);
        break;
    case EV_STOP:
#if CONFIG_FIREFLY_BUS_PROBE
        ESP_LOGW(TAG, "PROBE STOPPED: %s (tx_ok=%" PRIu32 " tx_fail=%" PRIu32 ")",
                 ev->sa < sizeof(k_stop_text) / sizeof(k_stop_text[0]) ? k_stop_text[ev->sa] : "?",
                 s_probe_tx_ok, s_probe_tx_fail);
#endif
        break;
    default:
        break;
    }
}

static void dump_table(void)
{
    ESP_LOGI(TAG, "TABLE begin keys=%u", (unsigned)s_sv.used);
    for (size_t i = 0; i < s_sv.used; i++) {
        const survey_entry_t *e = &s_tab[i];
        char hx[17];
        hex8(hx, e->last, e->dlc);
        const uint32_t avg = (e->count > 1) ? (e->last_ms - e->first_ms) / (e->count - 1u) : 0u;
        ESP_LOGI(TAG, "T %05" PRIX32 " %02X %02X n=%" PRIu32 " avg=%" PRIu32 " min=%" PRIu32
                 " max=%" PRIu32 " chg=%u d=%s",
                 e->dgn, e->sa, e->b0, e->count, avg,
                 e->min_dt_ms == UINT32_MAX ? 0u : e->min_dt_ms, e->max_dt_ms,
                 (unsigned)e->changes, hx);
    }
    ESP_LOGI(TAG, "TABLE end");
}

static void dump_sources(void)
{
    uint32_t frames[256] = { 0 };
    uint16_t keys[256] = { 0 };
    uint32_t first[256];
    uint32_t last[256] = { 0 };
    memset(first, 0xFF, sizeof(first));
    for (size_t i = 0; i < s_sv.used; i++) {
        const survey_entry_t *e = &s_tab[i];
        frames[e->sa] += e->count;
        keys[e->sa]++;
        if (e->first_ms < first[e->sa]) {
            first[e->sa] = e->first_ms;
        }
        if (e->last_ms > last[e->sa]) {
            last[e->sa] = e->last_ms;
        }
    }
    ESP_LOGI(TAG, "SOURCES begin");
    for (unsigned sa = 0; sa < 256; sa++) {
        if (keys[sa] != 0) {
            ESP_LOGI(TAG, "SA %02X frames=%" PRIu32 " keys=%u first_s=%" PRIu32 " last_s=%" PRIu32,
                     sa, frames[sa], (unsigned)keys[sa],
                     (first[sa] - s_sv.start_ms) / 1000u, (last[sa] - s_sv.start_ms) / 1000u);
        }
    }
    ESP_LOGI(TAG, "SOURCES end");
}

static void dump_deltas(void)
{
    if (!s_sv.have_baseline) {
        ESP_LOGW(TAG, "DELTAS: no baseline yet (press b)");
        return;
    }
    survey_delta_t d[32];
    const size_t n = survey_deltas(&s_sv, now_ms(), d, 32, DELTA_MIN_COUNT, DELTA_PCT);
    ESP_LOGI(TAG, "DELTAS vs baseline (%" PRIu32 " s ago, >%u%% rate change or new key): %u",
             (now_ms() - s_sv.baseline_ms) / 1000u, (unsigned)DELTA_PCT, (unsigned)n);
    for (size_t i = 0; i < n; i++) {
        const survey_entry_t *e = &s_tab[d[i].idx];
        ESP_LOGI(TAG, "D %s %05" PRIX32 " sa=%02X b0=%02X base=%" PRIu32 "mHz now=%" PRIu32 "mHz",
                 d[i].is_new ? "NEW " : "RATE", e->dgn, e->sa, e->b0, d[i].base_mhz, d[i].now_mhz);
    }
}

static void print_summary(uint32_t interval_ms, uint32_t frames_delta, uint32_t bits_delta,
                          uint32_t peak_fps)
{
    twai_status_info_t st = { 0 };
    twai_get_status_info(&st);
    const uint32_t fps = interval_ms ? (frames_delta * 1000u) / interval_ms : 0u;
    const uint32_t load_x10 = interval_ms ? (uint32_t)(((uint64_t)bits_delta * 4u) / interval_ms) : 0u;
    ESP_LOGI(TAG,
             "SUM t=%" PRIu32 "s frames=%" PRIu32 " fps=%" PRIu32 " peak=%" PRIu32
             " load=%" PRIu32 ".%" PRIu32 "%% keys=%u/%u dropfull=%" PRIu32 " std=%" PRIu32
             " evdrop=%" PRIu32 " | twai st=%d rxq=%" PRIu32 " miss=%" PRIu32 " ovr=%" PRIu32
             " txfail=%" PRIu32 " arb=%" PRIu32 " buserr=%" PRIu32 " txerr=%" PRIu32
             " rxerr=%" PRIu32 " probe=%u",
             (now_ms() - s_sv.start_ms) / 1000u, s_sv.total_frames, fps, peak_fps,
             load_x10 / 10u, load_x10 % 10u, (unsigned)s_sv.used, (unsigned)CAP,
             s_sv.dropped_full, s_std_frames, s_ev_dropped,
             (int)st.state, st.msgs_to_rx, st.rx_missed_count, st.rx_overrun_count,
             st.tx_failed_count, st.arb_lost_count, st.bus_error_count,
             st.tx_error_counter, st.rx_error_counter, s_probe_stage);
}

static void print_help(void)
{
    ESP_LOGI(TAG, "commands: h help | t table | a sources | b baseline | d deltas | c clear | "
                  "p probe status | 1 probe stage 1 | 2 probe stage 2 | x STOP probe");
}

static void handle_cmd(int c)
{
    switch (c) {
    case 'h': print_help(); break;
    case 't': dump_table(); break;
    case 'a': dump_sources(); break;
    case 'd': dump_deltas(); break;
    case 'b':
        survey_set_baseline(&s_sv, now_ms());
        ESP_LOGI(TAG, "BASELINE set at t=%" PRIu32 "s, %u keys",
                 (now_ms() - s_sv.start_ms) / 1000u, (unsigned)s_sv.used);
        break;
    case 'c':
        s_clear_req = true;
        ESP_LOGW(TAG, "survey cleared (restarts at the next frame)");
        break;
#if CONFIG_FIREFLY_BUS_PROBE
    case 'p': probe_report(); break;
    case '1': probe_start(1); break;
    case '2': probe_start(2); break;
    case 'x':
        if (s_probe_stage == 0) {
            ESP_LOGI(TAG, "probe is idle");
        }
        break;   /* the stop itself already happened in cmd_task */
#else
    case 'p': case '1': case '2': case 'x':
        ESP_LOGW(TAG, "probe not compiled into this build (CONFIG_FIREFLY_BUS_PROBE)");
        break;
#endif
    default:
        break;
    }
}

static void report_task(void *arg)
{
    (void)arg;
    uint32_t last_sec = now_ms(), last_sum = now_ms(), last_full = now_ms();
    uint32_t sec_frames = 0, sum_frames = 0, sum_bits = 0, peak = 0;

    print_help();

    for (;;) {
        ev_t ev;
        if (xQueueReceive(s_ev_q, &ev, pdMS_TO_TICKS(100)) == pdTRUE) {
            print_event(&ev);
        }
        int c;
        while (xQueueReceive(s_cmd_q, &c, 0) == pdTRUE) {
            handle_cmd(c);
        }

        const uint32_t t = now_ms();
        if (t - last_sec >= 1000u) {
            const uint32_t total = s_sv.total_frames;
            const uint32_t fps = total - sec_frames;
            sec_frames = total;
            if (fps > peak) {
                peak = fps;
            }
            last_sec = t;
        }
        if (t - last_sum >= REPORT_MS) {
            const uint32_t total = s_sv.total_frames;
            const uint32_t bits = s_bits_total;
            print_summary(t - last_sum, total - sum_frames, bits - sum_bits, peak);
            sum_frames = total;
            sum_bits = bits;
            peak = 0;
            last_sum = t;
        }
#if CONFIG_FIREFLY_BUS_SURVEY_TABLE_S > 0
        if (t - last_full >= FULL_REPORT_MS) {
            dump_sources();
            dump_table();
            if (s_sv.have_baseline) {
                dump_deltas();
            }
            last_full = t;
        }
#endif
    }
}

/* One byte from the UART console (stdin) or, failing that, the USB FIFO; EOF if neither. */
static int read_console_byte(void)
{
    const int c = getchar();
    if (c != EOF) {
        return c;
    }
#ifdef SURVEY_USB_RX
    if (usb_serial_jtag_ll_rxfifo_data_available()) {
        uint8_t b;
        if (usb_serial_jtag_ll_read_rxfifo(&b, 1) == 1) {
            return b;
        }
    }
#endif
    return EOF;
}

static void cmd_task(void *arg)
{
    (void)arg;
    for (;;) {
        const int c = read_console_byte();
        if (c == EOF) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (c == '\r' || c == '\n' || c == ' ') {
            continue;
        }
#if CONFIG_FIREFLY_BUS_PROBE
        if (c == 'x' || c == 'X') {
            probe_stop(STOP_OPERATOR);   /* immediate, independent of the reporter */
        }
#endif
        const int lc = (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c;
        xQueueSend(s_cmd_q, &lc, 0);
    }
}

esp_err_t bus_survey_start(void)
{
    s_tab = heap_caps_calloc(CAP, sizeof(survey_entry_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_tab == NULL) {
        ESP_LOGW(TAG, "no PSRAM for the table, using internal RAM");
        s_tab = heap_caps_calloc(CAP, sizeof(survey_entry_t), MALLOC_CAP_8BIT);
    }
    if (s_tab == NULL) {
        return ESP_ERR_NO_MEM;
    }
    survey_init(&s_sv, s_tab, CAP, now_ms());

    s_ev_q = xQueueCreate(EV_QUEUE_LEN, sizeof(ev_t));
    s_cmd_q = xQueueCreate(CMD_QUEUE_LEN, sizeof(int));
    if (s_ev_q == NULL || s_cmd_q == NULL) {
        return ESP_ERR_NO_MEM;
    }

    setvbuf(stdin, NULL, _IONBF, 0);

    if (xTaskCreatePinnedToCore(report_task, "survey_rpt", 6144, NULL, 2, NULL, 0) != pdPASS ||
        xTaskCreatePinnedToCore(cmd_task, "survey_cmd", 3072, NULL, 3, NULL, 0) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
#if CONFIG_FIREFLY_BUS_PROBE
    probe_build_schedule();
    if (xTaskCreatePinnedToCore(probe_task, "survey_probe", 3072, NULL, 2, NULL, 0) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
#endif

    ESP_LOGW(TAG, "BUS SURVEY BUILD: %d entries (%u KB), summary every %d s, probe %s; "
                  "internal RAM free %u B",
             (int)CAP, (unsigned)(CAP * sizeof(survey_entry_t) / 1024u),
             CONFIG_FIREFLY_BUS_SURVEY_REPORT_S,
#if CONFIG_FIREFLY_BUS_PROBE
             "compiled in (idle)",
#else
             "not compiled in",
#endif
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    return ESP_OK;
}

#endif /* CONFIG_FIREFLY_BUS_SURVEY */

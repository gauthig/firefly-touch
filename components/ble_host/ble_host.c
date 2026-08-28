#include "ble_host.h"

#include <stdbool.h>
#include <string.h>

#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_log.h"

static const char *TAG = "ble_host";

/* Three clients today (batteries, Power Watchdog, solar MPPT); the cap
 * exists to keep the tables static, not because more would be a problem.
 * Note the batteries register a GATTC observer only -- they connect to
 * fixed MACs and never scan -- so the GAP table is the emptier of the
 * two. */
#define BLE_HOST_MAX_OBSERVERS 4

static esp_gap_ble_cb_t   s_gap_observers[BLE_HOST_MAX_OBSERVERS];
static uint8_t            s_gap_count;
static esp_gattc_cb_t     s_gattc_observers[BLE_HOST_MAX_OBSERVERS];
static uint8_t            s_gattc_count;
static bool               s_started;

static void scan_ensure(void);

/* ------------------------------------------------- scan arbitration ----- *
 *
 * One scan for the whole node. See the header for why clients must not
 * drive esp_ble_gap_*_scanning() themselves.
 */

#define BLE_HOST_MAX_MATCHERS 4
/* Bounded windows rather than one endless scan: a window that ends gives a
 * natural retry point, so a device powered up later (or briefly out of
 * range) is picked up on the next one instead of depending on a single scan
 * never being disturbed. */
#define BLE_HOST_SCAN_WINDOW_S 30

typedef struct {
    ble_host_scan_match_t match;
    ble_host_scan_found_t found;
    void                 *ctx;
    bool                  want;        /* still looking for an address */
    bool                  pending;     /* matched; deliver once the scan stops */
    esp_bd_addr_t         bda;
    esp_ble_addr_type_t   addr_type;
} scan_matcher_t;

static scan_matcher_t s_matchers[BLE_HOST_MAX_MATCHERS];
static uint8_t        s_matcher_count;

/*
 * Names seen this scan window that nobody claimed, so each is logged once
 * rather than on every advertisement report.
 *
 * This exists because "my device was not found" and "my device was found but
 * the name did not match" look identical from the outside -- the scan just
 * runs forever and nothing happens. Printing what IS in range turns that
 * into a one-line answer, and it is the same trick the Renogy reference
 * implementation uses ("Possible device found! ..."). Bounded and reset per
 * window, so it cannot grow into a log flood.
 */
#define BLE_HOST_SEEN_MAX      12
#define BLE_HOST_SEEN_NAME_LEN 32
static char    s_seen[BLE_HOST_SEEN_MAX][BLE_HOST_SEEN_NAME_LEN];
static uint8_t s_seen_count;
static bool           s_scan_params_set;
static bool           s_scan_params_pending;
static bool           s_scanning;

static esp_ble_scan_params_t s_scan_params = {
    /* ACTIVE, not passive: the name we match on frequently arrives in the
     * scan response rather than in the advertisement itself. */
    .scan_type          = BLE_SCAN_TYPE_ACTIVE,
    .own_addr_type      = BLE_ADDR_TYPE_PUBLIC,
    .scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL,
    .scan_interval      = 0x50,
    .scan_window        = 0x30,
    .scan_duplicate     = BLE_SCAN_DUPLICATE_DISABLE,
};

static uint8_t pending_scan_count(void)
{
    uint8_t n = 0;
    for (uint8_t i = 0; i < s_matcher_count; i++) {
        if (s_matchers[i].want) {
            n++;
        }
    }
    return n;
}

static bool anyone_wants_scan(void)
{
    return pending_scan_count() > 0;
}

/* Starts a scan if one is wanted and none is running. Setting the scan
 * parameters is asynchronous, so the very first call only kicks that off --
 * SCAN_PARAM_SET_COMPLETE comes back through here again. */
static void scan_ensure(void)
{
    if (!s_started || s_scanning || !anyone_wants_scan()) {
        return;
    }
    if (!s_scan_params_set) {
        /* Only once: two clients asking to be scanned for before the first
         * SCAN_PARAM_SET_COMPLETE arrives would otherwise set the parameters
         * twice, and the two completions would then race to start a scan. */
        if (s_scan_params_pending) {
            return;
        }
        const esp_err_t err = esp_ble_gap_set_scan_params(&s_scan_params);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "set_scan_params: %s", esp_err_to_name(err));
            return;
        }
        s_scan_params_pending = true;
        return;
    }
    const esp_err_t err = esp_ble_gap_start_scanning(BLE_HOST_SCAN_WINDOW_S);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "start_scanning: %s", esp_err_to_name(err));
        return;
    }
    s_seen_count = 0;
    /* Logged because a scan that never starts is otherwise indistinguishable
     * from one that starts and finds nothing -- and on this node the
     * difference is the whole question when a device does not turn up. */
    ESP_LOGI(TAG, "scanning %ds for %u device(s)",
             BLE_HOST_SCAN_WINDOW_S, (unsigned)pending_scan_count());
}

/*
 * Hands each matcher that hit its address, now that scanning has actually
 * stopped -- clients open GATT connections from here, and doing that with a
 * scan still running is exactly what the stop-then-connect ordering avoids.
 *
 * Delivery happens before the restart check, so a client that fails to
 * connect and immediately asks to be scanned for again is picked up by the
 * very next scan_ensure() rather than waiting out a whole window.
 */
static void scan_deliver_pending(void)
{
    for (uint8_t i = 0; i < s_matcher_count; i++) {
        if (!s_matchers[i].pending) {
            continue;
        }
        s_matchers[i].pending = false;
        if (s_matchers[i].found != NULL) {
            s_matchers[i].found(s_matchers[i].bda, s_matchers[i].addr_type,
                                s_matchers[i].ctx);
        }
    }
}

/* Resolves the advertised name into `out`. Tries the complete name first and
 * falls back to the shortened one, since which of the two a device puts in
 * its advertisement (vs. its scan response) varies by vendor. */
static bool resolve_adv_name(esp_ble_gap_cb_param_t *p, char *out, size_t out_len)
{
    static const esp_ble_adv_data_type k_types[] = {
        ESP_BLE_AD_TYPE_NAME_CMPL,
        ESP_BLE_AD_TYPE_NAME_SHORT,
    };

    for (size_t t = 0; t < sizeof(k_types) / sizeof(k_types[0]); t++) {
        uint8_t len = 0;
        uint8_t *name = esp_ble_resolve_adv_data(p->scan_rst.ble_adv, k_types[t], &len);
        if (name == NULL || len == 0) {
            continue;
        }
        size_t n = len < out_len - 1 ? len : out_len - 1;
        memcpy(out, name, n);
        out[n] = '\0';
        /*
         * ⚠️ Trim TRAILING whitespace. Devices pad the advertised name to a
         * fixed field width and the padding comes through as part of the
         * name: this coach's Renogy BT-2 advertises literally
         * "BT-TH-B00E7B91    ", four trailing spaces, so an exact-name
         * matcher compared against the label on the device never fires --
         * and the only symptom is a scan that runs forever finding nothing.
         *
         * Trailing only, never leading: a leading character can be real. The
         * Watchdog on this same node advertises as "APMD1CB0DE309" with a
         * meaningful leading 'A' (see hughes_wd_name_matches).
         */
        while (n > 0 && (out[n - 1] == ' ' || out[n - 1] == '\t' ||
                         out[n - 1] == '\r' || out[n - 1] == '\n')) {
            out[--n] = '\0';
        }
        return true;
    }
    return false;
}

static void scan_on_adv(esp_ble_gap_cb_param_t *p)
{
    char name[32];
    if (!resolve_adv_name(p, name, sizeof(name))) {
        return;
    }

    bool hit = false;
    for (uint8_t i = 0; i < s_matcher_count; i++) {
        scan_matcher_t *m = &s_matchers[i];
        if (!m->want || m->match == NULL || !m->match(name, m->ctx)) {
            continue;
        }
        memcpy(m->bda, p->scan_rst.bda, sizeof(esp_bd_addr_t));
        m->addr_type = p->scan_rst.ble_addr_type;
        /* Cleared here rather than by the client: a matcher still wanting
         * after it hit would re-match the same device on every subsequent
         * advertisement report and re-trigger a connect. */
        m->want    = false;
        m->pending = true;
        hit = true;
        ESP_LOGI(TAG, "scan matched '%s' at %02X:%02X:%02X:%02X:%02X:%02X (addr type %d)",
                 name, p->scan_rst.bda[0], p->scan_rst.bda[1], p->scan_rst.bda[2],
                 p->scan_rst.bda[3], p->scan_rst.bda[4], p->scan_rst.bda[5],
                 p->scan_rst.ble_addr_type);
    }

    if (hit) {
        /* Stop even if others are still waiting: delivery has to happen with
         * the scan down, and scan_ensure() restarts it immediately
         * afterwards for whoever is left. */
        esp_ble_gap_stop_scanning();
        return;
    }

    /* Nobody wanted it -- report it once per window so a device that IS in
     * range but advertising under an unexpected name is visible, instead of
     * being indistinguishable from one that is switched off. */
    for (uint8_t i = 0; i < s_seen_count; i++) {
        if (strcmp(s_seen[i], name) == 0) {
            return;
        }
    }
    if (s_seen_count < BLE_HOST_SEEN_MAX) {
        /* memcpy + explicit terminator rather than strncpy(): a name that
         * exactly fills the buffer would leave no room for the NUL, which is
         * what -Wstringop-truncation warns about, correctly. Same reason
         * espnow_link.c's copy_key() avoids strncpy. */
        const size_t n = strnlen(name, BLE_HOST_SEEN_NAME_LEN - 1);
        memcpy(s_seen[s_seen_count], name, n);
        s_seen[s_seen_count][n] = '\0';
        s_seen_count++;
        ESP_LOGI(TAG, "  in range (unclaimed): '%s' %02X:%02X:%02X:%02X:%02X:%02X rssi %d",
                 name, p->scan_rst.bda[0], p->scan_rst.bda[1], p->scan_rst.bda[2],
                 p->scan_rst.bda[3], p->scan_rst.bda[4], p->scan_rst.bda[5],
                 p->scan_rst.rssi);
    }
}

static void scan_on_gap_event(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *p)
{
    switch (event) {
    case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT:
        if (p->scan_param_cmpl.status != ESP_BT_STATUS_SUCCESS) {
            ESP_LOGW(TAG, "scan param set failed, status %d", p->scan_param_cmpl.status);
            s_scan_params_pending = false;
            break;
        }
        s_scan_params_set = true;
        s_scan_params_pending = false;
        scan_ensure();
        break;

    case ESP_GAP_BLE_SCAN_START_COMPLETE_EVT:
        s_scanning = (p->scan_start_cmpl.status == ESP_BT_STATUS_SUCCESS);
        if (!s_scanning) {
            ESP_LOGW(TAG, "scan start failed, status %d", p->scan_start_cmpl.status);
        }
        break;

    case ESP_GAP_BLE_SCAN_RESULT_EVT:
        if (p->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_RES_EVT) {
            scan_on_adv(p);
        } else if (p->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_CMPL_EVT) {
            /* The scan window ran out on its own. */
            s_scanning = false;
            scan_deliver_pending();
            scan_ensure();
        }
        break;

    case ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT:
        s_scanning = false;
        scan_deliver_pending();
        scan_ensure();
        break;

    default:
        break;
    }
}

int ble_host_scan_add_matcher(ble_host_scan_match_t match,
                              ble_host_scan_found_t found, void *ctx)
{
    if (match == NULL || s_matcher_count >= BLE_HOST_MAX_MATCHERS) {
        return -1;
    }
    const int handle = (int)s_matcher_count++;
    s_matchers[handle].match = match;
    s_matchers[handle].found = found;
    s_matchers[handle].ctx   = ctx;
    ESP_LOGI(TAG, "scan matcher %d registered", handle);
    return handle;
}

void ble_host_scan_want(int handle, bool want)
{
    if (handle < 0 || handle >= (int)s_matcher_count) {
        return;
    }
    s_matchers[handle].want = want;
    if (want) {
        s_matchers[handle].pending = false;
        scan_ensure();
    }
}

/* ------------------------------------------------------------ fan-out --- */

static void gap_fanout(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    /* The arbiter goes first: it owns the scan state machine, and no
     * observer should ever see a scan event before ble_host has updated
     * what it believes about the scan. */
    scan_on_gap_event(event, param);

    for (uint8_t i = 0; i < s_gap_count; i++) {
        s_gap_observers[i](event, param);
    }
}

static void gattc_fanout(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                         esp_ble_gattc_cb_param_t *param)
{
    for (uint8_t i = 0; i < s_gattc_count; i++) {
        s_gattc_observers[i](event, gattc_if, param);
    }
}

esp_err_t ble_host_add_gap_observer(esp_gap_ble_cb_t cb)
{
    if (cb == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_gap_count >= BLE_HOST_MAX_OBSERVERS) {
        return ESP_ERR_NO_MEM;
    }
    s_gap_observers[s_gap_count++] = cb;
    ESP_LOGI(TAG, "GAP observer %u registered", (unsigned)s_gap_count);
    return ESP_OK;
}

esp_err_t ble_host_add_gattc_observer(esp_gattc_cb_t cb)
{
    if (cb == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_gattc_count >= BLE_HOST_MAX_OBSERVERS) {
        return ESP_ERR_NO_MEM;
    }
    s_gattc_observers[s_gattc_count++] = cb;
    ESP_LOGI(TAG, "GATTC observer %u registered", (unsigned)s_gattc_count);
    return ESP_OK;
}

esp_err_t ble_host_start(void)
{
    if (s_started) {
        return ESP_OK;
    }

    /* Classic BT memory is dead weight on a BLE-only node — and on the
     * classic ESP32 (unlike the S3) it is a real BR/EDR radio's worth of
     * RAM. ESP_ERR_INVALID_STATE just means it is already released. */
    esp_err_t err = esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "esp_bt_controller_mem_release: %s", esp_err_to_name(err));
    }

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    err = esp_bt_controller_init(&bt_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_bt_controller_init: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_bt_controller_enable: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_bluedroid_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_bluedroid_init: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_bluedroid_enable();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_bluedroid_enable: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_ble_gap_register_callback(gap_fanout);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ble_gap_register_callback: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_ble_gattc_register_callback(gattc_fanout);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ble_gattc_register_callback: %s", esp_err_to_name(err));
        return err;
    }

    s_started = true;
    /* A client may have asked to be scanned for before the stack was up --
     * registration order between clients is not guaranteed. Honour that now
     * rather than leaving it waiting on a scan nobody is going to start. */
    scan_ensure();
    /* Deliberately no observer counts here: a client that starts the host
     * registers before this point, and any client starting later registers
     * after it, so a count logged now would undercount and read as a bug
     * that isn't one. Observers announce themselves as they are added. */
    ESP_LOGI(TAG, "BLE host up");
    return ESP_OK;
}

/*
 * espnow_link — peer-to-peer ESP-NOW link between a CAN-connected "bridge"
 * panel and a remote panel with no CAN wiring at all.
 *
 * v1 is a single fixed peer (MAC + PMK/LMK from Kconfig, see
 * Kconfig.projbuild) on both ends — no runtime pairing, no mesh, no more
 * than one remote per bridge. A bridge panel calls espnow_link_init(true)
 * and uses espnow_link_send_status()/espnow_link_set_cmd_rx_cb(); a remote
 * panel calls espnow_link_init(false) and uses
 * espnow_link_send_cmd()/espnow_link_set_status_rx_cb().
 *
 * Mirrors the twai_tasks.c producer/consumer split: the ESP-NOW recv
 * callback (WiFi driver task context) only posts to a queue, never touches
 * application state directly — espnow_rx_task drains it and invokes the
 * registered callback.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "rvc_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Mirrors dimmer_cmd_msg_t (main/app_msgs.h) — kept separate so this
 * component has no dependency on main/. */
typedef struct {
    uint8_t          instance;
    rvc_dimmer_cmd_t command;
    uint8_t          level;      /* 0..200, or RVC_FIELD_NA */
    uint8_t          duration;   /* seconds, or RVC_FIELD_NA */
} espnow_cmd_msg_t;

/* Mirrors dimmer_status_msg_t. */
typedef struct {
    uint8_t instance;
    uint8_t level;
    bool    on;
} espnow_status_msg_t;

/* ------------------------------------------------------- telemetry ------ *
 *
 * Read-only measurements BROADCAST to every node on the channel, so any
 * panel can display them without being anybody's configured peer. This is a
 * separate traffic class from the command/status link above, in two ways
 * that matter:
 *
 *  - It is UNENCRYPTED, and unavoidably so: ESP-NOW cannot encrypt
 *    broadcast frames at all. That is acceptable here only because these
 *    frames are read-only telemetry. Command/status frames actuate real
 *    loads and stay encrypted unicast -- do not "simplify" by moving them
 *    onto the broadcast channel.
 *  - It is a SEPARATE FRAME STRUCT, deliberately. Adding telemetry to
 *    espnow_frame_t's union would change that struct's size, and the RX
 *    path validates length -- so a node flashed with the new firmware would
 *    silently drop command frames from a node still running the old one.
 *    Keeping the control frame byte-identical means panels can be updated
 *    one at a time.
 *
 * Values are scaled integers rather than floats: smaller frames, and no
 * dependence on float layout matching across two different chip families
 * (the basement proxy is a classic ESP32, the panels are ESP32-S3).
 */

typedef enum {
    ESPNOW_TELEM_SHORE_POWER = 1,   /* Hughes Power Watchdog, from the proxy */
    ESPNOW_TELEM_TANK        = 2,   /* RV-C TANK_STATUS, relayed by the bridge */
    ESPNOW_TELEM_BATTERY     = 3,   /* one JBD/Xiaoxiang pack, from the proxy */
    ESPNOW_TELEM_SOLAR       = 4,   /* Renogy MPPT controller, from the proxy */
} espnow_telem_kind_t;

typedef struct {
    uint8_t  line_count;        /* 1 = 30 A service, 2 = 50 A */
    uint8_t  error_code;        /* 0 = OK, 1..9 = E1..E9, 11/12 = F1/F2 */
    uint16_t frequency_chz;     /* Hz * 100 */
    uint16_t volts_dv[2];       /* V * 10, index 0 = L1 */
    uint16_t amps_da[2];        /* A * 10 */
    uint16_t watts[2];          /* W */
} espnow_shore_power_t;

typedef struct {
    uint8_t instance;           /* RV-C TANK_STATUS instance */
    uint8_t percent;
    uint8_t valid;              /* 0 = no reading; show "--", not 0 % */
} espnow_tank_msg_t;

/*
 * ONE battery pack, not the combined bank. The proxy holds all three BLE
 * links, but it deliberately broadcasts them separately and lets the panel
 * run jbd_bms_combine(): that keeps the aggregation in one pure-C,
 * host-tested place instead of forking it per producer, and it keeps the
 * panel's per-pack detail popup fed with real per-pack numbers.
 *
 * Temperatures are reduced to this pack's own min/max rather than every NTC
 * probe -- that is all either the popup or the bank's high/low strip shows,
 * and it is what keeps this struct inside its 16-byte budget (see the
 * _Static_assert in espnow_link.c: growing the telemetry union would make
 * an updated panel silently drop tank broadcasts from a mid_coach still
 * running the older build).
 */
typedef struct {
    uint8_t slot;               /* 0-based battery index on the producer */
    uint8_t flags;              /* bit0 = online, bit1 = temperatures valid */
    uint8_t soc_percent;
    uint8_t reserved;
    uint16_t volts_cv;          /* V * 100 */
    int16_t  current_da;        /* A * 10, signed: positive = charging */
    uint16_t residual_dah;      /* Ah * 10 */
    uint16_t full_dah;          /* Ah * 10 */
    int16_t  temp_min_dc;       /* degrees C * 10 */
    int16_t  temp_max_dc;
} espnow_battery_msg_t;

#define ESPNOW_BATTERY_FLAG_ONLINE     0x01u
#define ESPNOW_BATTERY_FLAG_TEMP_VALID 0x02u

/*
 * The Renogy MPPT charge controller, read over BLE by the proxy.
 *
 * ⚠️ EXACTLY 16 BYTES, which is the whole design constraint. The battery
 * message is the widest existing member, so this one has to fit the same
 * envelope -- see the _Static_assert on espnow_telem_frame_t in
 * espnow_link.c. Widening the union would make every telemetry broadcast
 * from a producer still running older firmware fail this receiver's length
 * check, silently killing tank and battery display until every node was
 * reflashed. If a new field will not fit, something else has to go.
 *
 * TEMPERATURES ARE IN °F, already converted on the producer. The controller
 * reports whole °C; the tenths here are load-bearing rather than decorative,
 * since x9/5 turns 33 °C into 91.4 °F and rounding to whole degrees would
 * throw that away. Converting once at the source also means a panel never
 * has to know which unit arrived.
 *
 * charge_state distinguishes "0 W because it is dark" from "0 W because the
 * battery is already full", which a bare wattage cannot express.
 */
typedef struct {
    uint8_t  flags;             /* bit0 = online */
    uint8_t  charge_state;      /* renogy_charge_state_t: 2 = mppt, 5 = float */
    uint8_t  battery_soc;       /* % */
    uint8_t  reserved;          /* explicit, so the u16s below stay aligned */
    uint16_t battery_volts_cv;  /* V * 100 */
    uint16_t pv_volts_cv;       /* V * 100 */
    uint16_t pv_current_ca;     /* A * 100 */
    uint16_t pv_watts;          /* W */
    int16_t  controller_temp_df; /* degrees F * 10, signed */
    int16_t  battery_temp_df;    /* degrees F * 10, signed */
} espnow_solar_msg_t;

#define ESPNOW_SOLAR_FLAG_ONLINE 0x01u

typedef struct {
    uint8_t kind;               /* espnow_telem_kind_t */
    union {
        espnow_shore_power_t shore;
        espnow_tank_msg_t    tank;
        espnow_battery_msg_t battery;
        espnow_solar_msg_t   solar;
    };
} espnow_telem_msg_t;

/*
 * ------------------------------------------------------- valve control ---
 *
 * A THIRD traffic class, separate from both the dimmer cmd/status frame and
 * telemetry: mid_coach commands the DrainMaster valve node (valves/) and
 * gets its position back. Kept as its own frame type for the same reason
 * telemetry is separate from dimmer cmd/status -- growing an existing
 * union would silently break compatibility with a node still on older
 * firmware. This one actuates a real motor, so unlike telemetry it stays
 * unicast and encrypted, same as the dimmer cmd/status frame.
 *
 * Doc's own UX language groups "unknown" and "fault" into one visual state
 * ("three visual states, not two"), so the wire only needs three position
 * values, not four -- see docs/DRAINMASTER-VALVES.md #8.
 */
typedef enum {
    ESPNOW_VALVE_ACTION_CLOSE = 0,
    ESPNOW_VALVE_ACTION_OPEN  = 1,
} espnow_valve_action_t;

typedef struct {
    uint8_t valve;   /* 0 = grey, 1 = black */
    uint8_t action;  /* espnow_valve_action_t */
} espnow_valve_cmd_msg_t;

typedef enum {
    ESPNOW_VALVE_POS_UNKNOWN = 0,   /* also covers "fault" -- see above */
    ESPNOW_VALVE_POS_CLOSED  = 1,
    ESPNOW_VALVE_POS_OPEN    = 2,   /* timed, never sensor-confirmed */
} espnow_valve_position_t;

typedef struct {
    uint8_t valve;
    uint8_t position;  /* espnow_valve_position_t */
} espnow_valve_status_msg_t;

typedef void (*espnow_cmd_rx_cb_t)(const espnow_cmd_msg_t *msg, void *ctx);
typedef void (*espnow_status_rx_cb_t)(const espnow_status_msg_t *msg, void *ctx);
typedef void (*espnow_telem_rx_cb_t)(const espnow_telem_msg_t *msg, void *ctx);
typedef void (*espnow_valve_cmd_rx_cb_t)(const espnow_valve_cmd_msg_t *msg, void *ctx);
typedef void (*espnow_valve_status_rx_cb_t)(const espnow_valve_status_msg_t *msg, void *ctx);

typedef enum {
    /* CAN panel: relays remote commands onto the bus, sends status back,
     * and may also broadcast tank telemetry. */
    ESPNOW_ROLE_BRIDGE = 0,
    /* Display panel with no CAN: sends commands, receives status, and
     * receives telemetry broadcasts. */
    ESPNOW_ROLE_REMOTE,
    /* Headless producer (the basement BLE proxy): broadcasts telemetry
     * only. Has no unicast peer at all, so CONFIG_FIREFLY_ESPNOW_PEER_MAC
     * is not consulted and may stay at its placeholder. */
    ESPNOW_ROLE_TELEMETRY,
    /* Headless DrainMaster valve controller (valves/): receives valve
     * commands, sends valve status. Its ONE peer is mid_coach, via the
     * normal CONFIG_FIREFLY_ESPNOW_PEER_MAC/LMK -- this is a distinct role
     * only so its own boot log doesn't call itself a "bridge". mid_coach's
     * side of this relationship is its SECOND peer -- see
     * espnow_link_add_valve_peer() below, not this enum. */
    ESPNOW_ROLE_VALVE_NODE,
} espnow_role_t;

/*
 * Brings up WiFi in STA-no-connect mode + ESP-NOW, adds the peers this role
 * needs, and starts espnow_rx_task. Must be called after nvs/board init;
 * safe to call once per boot.
 */
esp_err_t espnow_link_init(espnow_role_t role);

/* Remote panel -> bridge: send a dimmer command to be relayed onto CAN. */
bool espnow_link_send_cmd(const espnow_cmd_msg_t *msg);

/* Bridge -> remote panel: relay a real DC_DIMMER_STATUS_3 change. */
bool espnow_link_send_status(const espnow_status_msg_t *msg);

/* Broadcast one telemetry measurement to every node on the channel. */
bool espnow_link_send_telemetry(const espnow_telem_msg_t *msg);

/* True if any frame was received from the peer within the last 5 s. */
bool espnow_link_healthy(void);

/* Bridge role: called from espnow_rx_task when a command arrives. */
void espnow_link_set_cmd_rx_cb(espnow_cmd_rx_cb_t cb, void *ctx);

/* Remote role: called from espnow_rx_task when a status update arrives. */
void espnow_link_set_status_rx_cb(espnow_status_rx_cb_t cb, void *ctx);

/* Any receiving role: called when a telemetry broadcast arrives. */
void espnow_link_set_telem_rx_cb(espnow_telem_rx_cb_t cb, void *ctx);

/*
 * ------------------------------------------------------- valve control ---
 *
 * mid_coach-side only: adds a SECOND unicast peer (the valve node) on top
 * of whatever espnow_link_init() already set up as the primary peer
 * (bedroom_remote). Reads CONFIG_FIREFLY_ESPNOW_VALVE_PEER_MAC and
 * CONFIG_FIREFLY_ESPNOW_VALVE_LMK -- a separate MAC/LMK pair from the
 * primary peer's, reusing only the network-wide PMK and channel. Call
 * after espnow_link_init().
 */
esp_err_t espnow_link_add_valve_peer(void);

/* mid_coach -> valve node, over the second peer added above. */
bool espnow_link_send_valve_cmd(const espnow_valve_cmd_msg_t *msg);

/* Valve node -> mid_coach, over its one (only) peer. */
bool espnow_link_send_valve_status(const espnow_valve_status_msg_t *msg);

/* Valve node role: called from espnow_rx_task when a command arrives. */
void espnow_link_set_valve_cmd_rx_cb(espnow_valve_cmd_rx_cb_t cb, void *ctx);

/* mid_coach: called from espnow_rx_task when valve status arrives. */
void espnow_link_set_valve_status_rx_cb(espnow_valve_status_rx_cb_t cb, void *ctx);

#ifdef __cplusplus
}
#endif

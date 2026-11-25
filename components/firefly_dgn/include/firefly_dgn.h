/*
 * firefly_dgn — this coach's own RV-C DGN family, 0FFC0..0FFCF.
 *
 * Pure C, no ESP / easytouch / jbd dependencies (host-testable, like
 * rvc_protocol). The byte layouts are specified in docs/RVC-CUSTOM-DGNS.md;
 * this header is their single implementation, so producers (the stool-room
 * gateway) and consumers (the CAN panels) cannot disagree about a field.
 *
 * Conventions (spec sec. 3):
 *   - 8-byte frames, little-endian, byte 0 is the instance.
 *   - Not available: unsigned all ones (0xFF / 0xFFFF), signed 0x7F / 0x7FFF.
 *   - Reserved bytes are 0xFF.
 *   - Scaled quantities travel as integers; in these structs they are floats
 *     in engineering units, with NAN meaning "not available". Encoding rounds
 *     to the nearest step and saturates at the field limits; it never wraps.
 *
 * The block is J1939 proprietary-B space (PDU2, data page 0). Issue #96 showed
 * it is unused on this coach and that the G6 ignores it.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FDGN_LEN 8u

typedef enum {
    FDGN_HVAC_STATUS    = 0x0FFC0,
    FDGN_HVAC_EXTRAS    = 0x0FFC1,
    FDGN_BATT_PACK1     = 0x0FFC2,
    FDGN_BATT_PACK2     = 0x0FFC3,
    FDGN_BATT_PACK3     = 0x0FFC4,
    FDGN_BANK_A         = 0x0FFC5,
    FDGN_SHORE_LINE     = 0x0FFC6,
    FDGN_SOLAR_A        = 0x0FFC7,
    FDGN_PACK_DETAIL    = 0x0FFC8,
    FDGN_BANK_B         = 0x0FFC9,
    FDGN_HVAC_CMD       = 0x0FFCA,
    FDGN_SOLAR_B        = 0x0FFCB,
    FDGN_SHORE_PEDESTAL = 0x0FFCC,
    FDGN_VALVE_STATUS   = 0x0FFCD,
    FDGN_VALVE_CMD      = 0x0FFCE,
    FDGN_GW_HEARTBEAT   = 0x0FFCF,
} fdgn_dgn_t;

#define FDGN_FIRST  0x0FFC0u
#define FDGN_LAST   0x0FFCFu

#define FDGN_U8_NA  0xFFu
#define FDGN_S8_NA  0x7F
#define FDGN_U16_NA 0xFFFFu
#define FDGN_S16_NA 0x7FFF

/* True for any DGN in our block. */
bool fdgn_is_ours(uint32_t dgn);

/* Battery packs each have their own DGN: pack 1..3 <-> 0FFC2..0FFC4. */
uint32_t fdgn_pack_dgn(uint8_t pack);          /* 0 if pack is not 1..3 */
uint8_t  fdgn_pack_from_dgn(uint32_t dgn);     /* 0 if not a pack DGN */

/* ------------------------------------------------------------- HVAC ---- */

/* 0FFC0, 2 s per zone. mode / current_mode / fan are EasyTouch-native values
 * (mode 4 = Aqua-Hot, 7 = electric heat), so the two heat sources stay apart. */
typedef struct {
    uint8_t zone;           /* 0..3 */
    bool    present;
    bool    fresh;          /* last BLE read < 90 s old */
    uint8_t mode;           /* 0 off, 1 fan, 2 cool, 4 Aqua-Hot, 7 electric heat, 11 auto */
    uint8_t current_mode;   /* 0 idle, 2 cooling, 4 heating */
    uint8_t fan;            /* fan for the selected mode, EasyTouch enum */
    int8_t  inside_f;       /* FDGN_S8_NA if unknown */
    uint8_t cool_sp_f;
    uint8_t heat_sp_f;
} fdgn_hvac_status_t;

/* 0FFC1, 10 s per zone. */
typedef struct {
    uint8_t zone;
    uint8_t auto_heat_sp_f;
    uint8_t auto_cool_sp_f;
    uint8_t age_s;          /* seconds since the last BLE read, saturates at 254 */
    uint8_t fan_only;
    uint8_t cool_fan;
    uint8_t heat_fan;
    uint8_t auto_fan;
} fdgn_hvac_extras_t;

/* 0FFCA, event driven, any panel -> gateway. */
typedef enum {
    FDGN_HVAC_OP_MODE         = 0,
    FDGN_HVAC_OP_COOL_SP      = 1,
    FDGN_HVAC_OP_HEAT_SP      = 2,
    FDGN_HVAC_OP_AUTO_HEAT_SP = 3,
    FDGN_HVAC_OP_AUTO_COOL_SP = 4,
    FDGN_HVAC_OP_COOL_FAN     = 5,
    FDGN_HVAC_OP_HEAT_FAN     = 6,
    FDGN_HVAC_OP_AUTO_FAN     = 7,
    FDGN_HVAC_OP_FAN_ONLY     = 8,
} fdgn_hvac_op_t;

typedef struct {
    uint8_t zone;
    uint8_t op;             /* fdgn_hvac_op_t */
    uint8_t arg;
} fdgn_hvac_cmd_t;

/* ---------------------------------------------------------- Battery ---- */

/* 0FFC2/3/4, 5 s each. Current is positive while charging. */
typedef struct {
    uint8_t pack;           /* 1..3 */
    bool    online;
    bool    temps_valid;
    uint8_t soc_pct;        /* FDGN_U8_NA if unknown */
    float   volts;          /* 0.01 V */
    float   amps;           /* 0.1 A, signed */
} fdgn_batt_pack_t;

/* 0FFC8, 10 s per pack. */
typedef struct {
    uint8_t pack;
    float   residual_ah;    /* 0.1 Ah */
    float   full_ah;        /* 0.1 Ah */
    float   temp_min_c;     /* compact: 0.5 C steps, -40 .. +87 */
    float   temp_max_c;
} fdgn_pack_detail_t;

/* 0FFC5, 5 s. */
typedef struct {
    uint8_t bank;           /* 1 */
    uint8_t packs_configured;
    uint8_t packs_online;
    uint8_t soc_pct;
    float   volts;
    float   amps;
} fdgn_bank_a_t;

typedef enum {
    FDGN_BANK_IDLE        = 0,
    FDGN_BANK_CHARGING    = 1,
    FDGN_BANK_DISCHARGING = 2,
} fdgn_bank_state_t;

/* 0FFC9, 5 s. */
typedef struct {
    uint8_t  bank;
    float    watts;         /* 1 W, signed, saturates at +/-32766 */
    uint16_t minutes_left;  /* FDGN_U16_NA when idle / unknown; saturates at 0xFFFE */
    float    temp_high_c;   /* compact */
    float    temp_low_c;
    uint8_t  state;         /* fdgn_bank_state_t */
} fdgn_bank_b_t;

/* ------------------------------------------------------------ Shore ---- */

/* 0FFC6, 2 s per line. */
typedef struct {
    uint8_t line;           /* 0 = A (L1), 1 = B (L2) */
    float   volts;          /* 0.1 V */
    float   amps;           /* 0.1 A */
    float   watts;          /* 1 W, saturates at 65534 */
    float   hz;             /* 40.0 + raw * 0.1, 40.0 .. 65.4 */
} fdgn_shore_line_t;

/* 0FFCC, 10 s. */
typedef struct {
    uint8_t line_count;     /* 1 = 30 A, 2 = 50 A */
    uint8_t error_code;     /* Watchdog: 0 OK, 1-9 E1-E9, 11/12 F1/F2 */
} fdgn_shore_pedestal_t;

/* ------------------------------------------------------------ Solar ---- */

/* 0FFC7, 5 s. */
typedef struct {
    uint8_t controller;     /* 1 */
    bool    online;
    uint8_t charge_state;   /* Renogy enum */
    uint8_t battery_soc_pct;
    float   battery_volts;  /* 0.01 V */
    float   pv_volts;       /* 0.01 V */
} fdgn_solar_a_t;

/* 0FFCB, 5 s. */
typedef struct {
    uint8_t controller;
    float   pv_amps;        /* 0.01 A */
    float   pv_watts;       /* 1 W */
    float   controller_temp_f;  /* F = raw - 40 */
    float   battery_temp_f;
} fdgn_solar_b_t;

/* ----------------------------------------------------------- Valves ---- */

typedef enum {
    FDGN_VALVE_UNKNOWN = 0,
    FDGN_VALVE_CLOSED  = 1,
    FDGN_VALVE_OPEN    = 2,
} fdgn_valve_position_t;

/* 0FFCD, on change + 15 s. */
typedef struct {
    uint8_t valve;          /* 0 grey, 1 black */
    uint8_t position;       /* fdgn_valve_position_t */
    uint8_t age_s;          /* since the valve node last reported, saturates at 254 */
    bool    node_fresh;     /* valve-node link alive */
} fdgn_valve_status_t;

typedef enum {
    FDGN_VALVE_ACT_CLOSE = 0,
    FDGN_VALVE_ACT_OPEN  = 1,
} fdgn_valve_action_t;

/* 0FFCE, event driven, any CAN panel -> gateway. */
typedef struct {
    uint8_t valve;
    uint8_t action;         /* fdgn_valve_action_t */
} fdgn_valve_cmd_t;

/* -------------------------------------------------------- Heartbeat ---- */

/* Link bitmap bits in the gateway heartbeat. */
#define FDGN_LINK_PACK1      0x01u
#define FDGN_LINK_PACK2      0x02u
#define FDGN_LINK_PACK3      0x04u
#define FDGN_LINK_WATCHDOG   0x08u
#define FDGN_LINK_SOLAR      0x10u
#define FDGN_LINK_THERMOSTAT 0x20u
#define FDGN_LINK_VALVE_NODE 0x40u

/* 0FFCF, 5 s. */
typedef struct {
    uint8_t  gateway_id;
    uint8_t  fw_major;
    uint8_t  fw_minor;
    uint16_t uptime_min;    /* saturates at 0xFFFE */
    uint8_t  links;         /* FDGN_LINK_* */
    uint8_t  health;        /* gateway-defined flags */
} fdgn_heartbeat_t;

/* ----------------------------------------------------- encode/decode --- */
/*
 * encode_* always writes all FDGN_LEN bytes of out.
 * decode_* returns false (leaving *out untouched) unless dlc == FDGN_LEN and
 * the instance byte is in range for that DGN.
 */
void fdgn_encode_hvac_status(const fdgn_hvac_status_t *in, uint8_t out[FDGN_LEN]);
bool fdgn_decode_hvac_status(const uint8_t *d, uint8_t dlc, fdgn_hvac_status_t *out);
void fdgn_encode_hvac_extras(const fdgn_hvac_extras_t *in, uint8_t out[FDGN_LEN]);
bool fdgn_decode_hvac_extras(const uint8_t *d, uint8_t dlc, fdgn_hvac_extras_t *out);
void fdgn_encode_hvac_cmd(const fdgn_hvac_cmd_t *in, uint8_t out[FDGN_LEN]);
bool fdgn_decode_hvac_cmd(const uint8_t *d, uint8_t dlc, fdgn_hvac_cmd_t *out);

void fdgn_encode_batt_pack(const fdgn_batt_pack_t *in, uint8_t out[FDGN_LEN]);
bool fdgn_decode_batt_pack(const uint8_t *d, uint8_t dlc, fdgn_batt_pack_t *out);
void fdgn_encode_pack_detail(const fdgn_pack_detail_t *in, uint8_t out[FDGN_LEN]);
bool fdgn_decode_pack_detail(const uint8_t *d, uint8_t dlc, fdgn_pack_detail_t *out);
void fdgn_encode_bank_a(const fdgn_bank_a_t *in, uint8_t out[FDGN_LEN]);
bool fdgn_decode_bank_a(const uint8_t *d, uint8_t dlc, fdgn_bank_a_t *out);
void fdgn_encode_bank_b(const fdgn_bank_b_t *in, uint8_t out[FDGN_LEN]);
bool fdgn_decode_bank_b(const uint8_t *d, uint8_t dlc, fdgn_bank_b_t *out);

void fdgn_encode_shore_line(const fdgn_shore_line_t *in, uint8_t out[FDGN_LEN]);
bool fdgn_decode_shore_line(const uint8_t *d, uint8_t dlc, fdgn_shore_line_t *out);
void fdgn_encode_shore_pedestal(const fdgn_shore_pedestal_t *in, uint8_t out[FDGN_LEN]);
bool fdgn_decode_shore_pedestal(const uint8_t *d, uint8_t dlc, fdgn_shore_pedestal_t *out);

void fdgn_encode_solar_a(const fdgn_solar_a_t *in, uint8_t out[FDGN_LEN]);
bool fdgn_decode_solar_a(const uint8_t *d, uint8_t dlc, fdgn_solar_a_t *out);
void fdgn_encode_solar_b(const fdgn_solar_b_t *in, uint8_t out[FDGN_LEN]);
bool fdgn_decode_solar_b(const uint8_t *d, uint8_t dlc, fdgn_solar_b_t *out);

void fdgn_encode_valve_status(const fdgn_valve_status_t *in, uint8_t out[FDGN_LEN]);
bool fdgn_decode_valve_status(const uint8_t *d, uint8_t dlc, fdgn_valve_status_t *out);
void fdgn_encode_valve_cmd(const fdgn_valve_cmd_t *in, uint8_t out[FDGN_LEN]);
bool fdgn_decode_valve_cmd(const uint8_t *d, uint8_t dlc, fdgn_valve_cmd_t *out);

void fdgn_encode_heartbeat(const fdgn_heartbeat_t *in, uint8_t out[FDGN_LEN]);
bool fdgn_decode_heartbeat(const uint8_t *d, uint8_t dlc, fdgn_heartbeat_t *out);

#ifdef __cplusplus
}
#endif

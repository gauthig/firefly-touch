/*
 * bus_survey — what is actually on the RV-C bus, over time.
 *
 * Pure C, no ESP dependencies (host-testable, like rvc_protocol): the ESP side
 * (main/bus_survey_task.c) feeds it frames and does all the printing.
 *
 * A table entry is one (canonical DGN, source address, data[0]) combination.
 * data[0] is the instance byte for nearly every RV-C DGN, so this separates
 * e.g. the 40-odd dimmer instances the G6 reports without needing to know
 * each DGN's layout. For DGNs where byte 0 is not an instance it simply
 * splits the key a little further, which is harmless for a survey.
 *
 * Single writer (the CAN rx task); the reporter reads the table without a
 * lock. A torn read can only misreport one diagnostic row, never corrupt
 * anything, so the rx path stays lock-free.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Well-known J1939/RV-C parameter groups the survey treats specially. */
#define SURVEY_DGN_ACK          0x0E800u   /* acknowledgment (G6 acks commands) */
#define SURVEY_DGN_REQUEST      0x0EA00u   /* request for a PGN / address claim */
#define SURVEY_DGN_ADDR_CLAIM   0x0EE00u   /* address claimed (carries the NAME) */
#define SURVEY_DGN_DM_RV        0x1FECAu   /* RV-C diagnostic message */
#define SURVEY_DGN_DM1          0x0FECAu   /* J1939 DM1: every device heartbeats it (seen on this coach) */

typedef struct {
    uint32_t dgn;          /* canonical: PDU1 destination byte stripped */
    uint8_t  sa;
    uint8_t  b0;           /* data[0], or 0xFF when the frame has no data */
    uint8_t  dlc;
    uint8_t  last[8];      /* most recent payload */
    uint16_t changes;      /* times the payload differed from the previous one */
    uint32_t count;
    uint32_t first_ms;
    uint32_t last_ms;
    uint32_t min_dt_ms;    /* smallest gap between two frames; UINT32_MAX until two seen */
    uint32_t max_dt_ms;    /* largest gap */
    uint32_t bl_count;     /* count at the moment the baseline was set */
} survey_entry_t;

typedef struct {
    survey_entry_t *e;     /* caller-provided storage */
    size_t   cap;
    size_t   used;
    uint32_t start_ms;
    uint32_t total_frames;
    uint32_t dropped_full; /* frames whose key did not fit in the table */
    bool     have_baseline;
    uint32_t baseline_ms;
} survey_t;

typedef enum {
    SURVEY_SEEN = 0,       /* known key, same payload */
    SURVEY_NEW,            /* first time this key was seen */
    SURVEY_CHANGED,        /* known key, payload differs from the last one */
    SURVEY_FULL,           /* table full, frame not recorded */
} survey_result_t;

void survey_init(survey_t *s, survey_entry_t *storage, size_t cap, uint32_t now_ms);

/* Strip the PDU1 destination byte so a DGN is one stable number. A 17-bit
 * DGN (data page + PF + PS) with PF < 0xF0 is destination-addressed and its low
 * byte is the destination, not part of the group. *da gets that destination, or
 * 0xFF for a PDU2 (broadcast) group. */
uint32_t survey_canonical_dgn(uint32_t dgn17, uint8_t *da);

/* Record one extended-ID frame. dgn17 is the raw (can_id >> 8) & 0x1FFFF. */
survey_result_t survey_note(survey_t *s, uint32_t now_ms, uint32_t dgn17, uint8_t sa,
                            uint8_t dlc, const uint8_t data[8]);

/* True if this canonical DGN has been seen from ANY source address. */
bool survey_dgn_seen(const survey_t *s, uint32_t canonical_dgn);

/* Freeze the current counts as the baseline for survey_deltas(). */
void survey_set_baseline(survey_t *s, uint32_t now_ms);

typedef struct {
    size_t   idx;          /* index into the table */
    uint32_t base_mhz;     /* baseline rate, milli-Hz (0 for a new key) */
    uint32_t now_mhz;      /* rate since the baseline, milli-Hz */
    bool     is_new;       /* key first appeared after the baseline */
} survey_delta_t;

/* Keys whose rate since the baseline differs from before by more than pct
 * percent, plus any key that first appeared after the baseline. Existing keys
 * need at least min_base_count frames before the baseline and both windows at
 * least 30 s long, otherwise a handful of frames would read as a rate change.
 * Returns the number written to out (at most max). */
size_t survey_deltas(const survey_t *s, uint32_t now_ms, survey_delta_t *out, size_t max,
                     uint32_t min_base_count, uint32_t pct);

/* On-wire bits of one extended CAN frame including an average stuffing
 * allowance. An estimate for bus-load reporting, not a bit-exact count. */
uint32_t survey_frame_bits(uint8_t dlc);

#ifdef __cplusplus
}
#endif

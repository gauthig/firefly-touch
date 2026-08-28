#include "rvc_protocol.h"

#include <string.h>

uint32_t rvc_id_pack(uint8_t priority, uint32_t dgn, uint8_t source_addr)
{
    return ((uint32_t)(priority & 0x7u) << 26)
         | ((dgn & 0x1FFFFu) << 8)
         | source_addr;
}

rvc_id_t rvc_id_unpack(uint32_t can_id)
{
    rvc_id_t id = {
        .priority    = (uint8_t)((can_id >> 26) & 0x7u),
        .dgn         = (can_id >> 8) & 0x1FFFFu,
        .source_addr = (uint8_t)(can_id & 0xFFu),
    };
    return id;
}

/*
 * DC_DIMMER_COMMAND_2 payload layout (RV-C):
 *   byte 0  instance
 *   byte 1  group bitmask (0xFF = no group addressing)
 *   byte 2  desired level, 0..200 in 0.5 % steps (0xFF = no change)
 *   byte 3  command (rvc_dimmer_cmd_t)
 *   byte 4  delay/duration in seconds (0xFF = none)
 *   byte 5  bits 0-1 = interlock command: 00 = no interlock, 01/10 =
 *           interlock A/B. Send 0x00 — an earlier revision sent 0xFF
 *           (bits 11 + all reserved bits set), which is NOT the spec
 *           "no interlock" value and is suspected to have contributed to
 *           G6A loads latching up. Every proven-working implementation
 *           (rvc-proxy, CoachProxy) sends 0x00 here.
 *   bytes 6..7  reserved, 0xFF
 *
 * Reference known-good frame (rvc-proxy dc_dimmer.pl, works on real
 * coaches):  ON  = [inst, FF, C8, 02, FF, 00, FF, FF]
 *            OFF = [inst, FF, C8, 03, FF, 00, FF, FF]
 *
 * TODO(bench): capture a factory Firefly switch press in sniffer mode and
 * compare byte 1 (group) against what the G6A controllers actually expect.
 */
void rvc_encode_dc_dimmer_command_2(const rvc_dimmer_command_t *cmd, uint8_t data[8])
{
    memset(data, 0xFF, 8);
    data[0] = cmd->instance;
    data[1] = cmd->group;
    /*
     * Clamp real brightnesses to 0..200, but let the SENTINEL values through
     * untouched.
     *
     * ⚠️ RVC_LEVEL_RESTORE (251) is the trap: it is numerically above
     * RVC_LEVEL_MAX but is not a brightness at all, it is "restore the
     * remembered level". Clamping it to 200 silently turns a master-on into
     * "set every light to 100 %" -- which is exactly what happened on the
     * coach 2026-08-28 when the light master was first switched to the real
     * group frames: the OFF half worked and the ON half lit loads that had
     * been off. Any future sentinel above 200 needs adding here too.
     */
    data[2] = (cmd->level > RVC_LEVEL_MAX &&
               cmd->level != RVC_FIELD_NA &&
               cmd->level != RVC_LEVEL_RESTORE)
                  ? RVC_LEVEL_MAX : cmd->level;
    data[3] = (uint8_t)cmd->command;
    data[4] = cmd->duration;
    data[5] = 0x00;   /* no interlock — never 0xFF, see above */
}

/*
 * DC_DIMMER_STATUS_3 payload layout (RV-C):
 *   byte 0  instance
 *   byte 1  group
 *   byte 2  operating level, 0..200
 *   byte 3  lock status (bits 0-1), rest reserved
 *   byte 4  delay/duration remaining
 *   byte 5  last command received
 *   byte 6  status bits: overcurrent (0-1), override (2-3), enable (4-5)
 *   byte 7  reserved
 *
 * TODO(bench): verify byte 3..6 semantics against real G6A STATUS_3 frames in
 * sniffer mode — field order past byte 2 varies between RV-C revisions.
 * load_on is derived from operating_level > 0, which is how the Firefly app
 * appears to treat it; confirm a load commanded ON at level 0 doesn't exist.
 */
bool rvc_decode_dc_dimmer_status_3(const uint8_t *data, size_t len, rvc_dimmer_status_t *out)
{
    if (data == NULL || out == NULL || len < 3) {
        return false;
    }

    memset(out, 0, sizeof(*out));
    out->instance        = data[0];
    out->group           = data[1];
    out->operating_level = data[2] > RVC_LEVEL_MAX ? RVC_LEVEL_MAX : data[2];
    out->lock_status         = len > 3 ? data[3] : RVC_FIELD_NA;
    out->duration_remaining  = len > 4 ? data[4] : RVC_FIELD_NA;
    out->last_command        = len > 5 ? data[5] : RVC_FIELD_NA;
    out->status_bits         = len > 6 ? data[6] : RVC_FIELD_NA;
    out->load_on = (data[2] != 0 && data[2] != RVC_FIELD_NA);
    return true;
}

/*
 * TANK_STATUS payload layout (RV-C), broadcast read-only by the Garnet
 * SeeLevel II 709-RVC tank monitor:
 *   byte 0    instance
 *   byte 1    relative level (count of "wet" capacitive sensor segments)
 *   byte 2    resolution (total sensor segment count for this tank's strip;
 *             percent = relative_level * 100 / resolution)
 *   byte 3-4  absolute level, Liters (uint16, unused here)
 *   byte 5-6  tank size, Liters (uint16, unused here)
 *
 * Corrected 2026-08-15 against a real bus capture on the coach (sniffer
 * mode) after this unit's tanks all showed 0%: resolution is NOT a
 * percent-per-count divisor (relative_level is always < resolution, so
 * integer-dividing them without the *100 always truncates to 0) -- it's the
 * SeeLevel's total segment count for that specific tank's sensor strip,
 * which is why it varies per instance (e.g. 32 vs 28 on this coach) rather
 * than being a fixed protocol constant. Captured frames, used verbatim as
 * regression test vectors in host_test/test_rvc.c:
 *   instance 0 (fresh): level=4,  resolution=32 -> 12%
 *   instance 1 (black): level=3,  resolution=28 -> 10%
 *   instance 2 (gray):  level=19, resolution=28 -> 67%
 *
 * relative_level == RVC_FIELD_NA (0xFF), or resolution == 0, means "not yet
 * reported" per docs/instance_map.yaml -> tank_dgn. Percent is clamped to
 * 0..100 defensively — an out-of-spec relative_level > resolution could
 * otherwise report over 100%.
 */
bool rvc_decode_tank_status(const uint8_t *data, size_t len, rvc_tank_status_t *out)
{
    if (data == NULL || out == NULL || len < 3) {
        return false;
    }

    memset(out, 0, sizeof(*out));
    out->instance = data[0];

    const uint8_t relative_level = data[1];
    const uint8_t resolution = data[2];
    if (relative_level == RVC_FIELD_NA || resolution == 0) {
        out->valid = false;
        return true;
    }

    const uint32_t percent = ((uint32_t)relative_level * 100u) / resolution;
    out->percent = (uint8_t)(percent > 100 ? 100 : percent);
    out->valid = true;
    return true;
}

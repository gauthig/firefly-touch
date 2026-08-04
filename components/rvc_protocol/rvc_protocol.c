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
 *   byte 5  interlock bits (we send 0xFF = no interlock / not used)
 *   bytes 6..7  reserved, 0xFF
 *
 * TODO(bench): capture a factory Firefly switch press in sniffer mode and
 * compare byte 1 (group) and byte 5 (interlock) against what the G6A
 * controllers actually expect. 0xFF for both is the spec "not used" value.
 */
void rvc_encode_dc_dimmer_command_2(const rvc_dimmer_command_t *cmd, uint8_t data[8])
{
    memset(data, 0xFF, 8);
    data[0] = cmd->instance;
    data[1] = cmd->group;
    data[2] = (cmd->level > RVC_LEVEL_MAX && cmd->level != RVC_FIELD_NA)
                  ? RVC_LEVEL_MAX : cmd->level;
    data[3] = (uint8_t)cmd->command;
    data[4] = cmd->duration;
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

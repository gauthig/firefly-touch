#include "bridge_tx.h"

#include "panel_config.h"

#if PANEL_HAS_CAN

#include "twai_tasks.h"

bool bridge_enqueue_dimmer_cmd(uint8_t instance, rvc_dimmer_cmd_t cmd,
                               uint8_t level, uint8_t duration)
{
    return twai_enqueue_dimmer_cmd(instance, cmd, level, duration);
}

bool bridge_enqueue_dimmer_group_cmd(uint8_t group, rvc_dimmer_cmd_t cmd,
                                     uint8_t level)
{
    return twai_enqueue_dimmer_group_cmd(group, cmd, level);
}

#else /* !PANEL_HAS_CAN — relay to the bridge panel over ESP-NOW */

#include "espnow_link.h"

bool bridge_enqueue_dimmer_cmd(uint8_t instance, rvc_dimmer_cmd_t cmd,
                               uint8_t level, uint8_t duration)
{
    const espnow_cmd_msg_t msg = {
        .instance = instance,
        .command = cmd,
        .level = level,
        .duration = duration,
    };
    return espnow_link_send_cmd(&msg);
}

/*
 * Group addressing does not cross the ESP-NOW link: espnow_cmd_msg_t has no
 * group field, and adding one would change a wire format that is pinned by a
 * _Static_assert and shared with panels flashed one at a time.
 *
 * Unreachable in practice — PANEL_BTN_LIGHT_MASTER is #error-guarded against
 * !PANEL_HAS_CAN — so this exists to keep the two branches symmetrical rather
 * than to be called. If a remote panel ever needs a master, relay it as an
 * explicit command the bridge expands, don't widen the frame.
 */
bool bridge_enqueue_dimmer_group_cmd(uint8_t group, rvc_dimmer_cmd_t cmd,
                                     uint8_t level)
{
    (void)group;
    (void)cmd;
    (void)level;
    return false;
}

#endif

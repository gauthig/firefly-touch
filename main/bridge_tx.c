#include "bridge_tx.h"

#include "panel_config.h"

#if PANEL_HAS_CAN

#include "twai_tasks.h"

bool bridge_enqueue_dimmer_cmd(uint8_t instance, rvc_dimmer_cmd_t cmd,
                               uint8_t level, uint8_t duration)
{
    return twai_enqueue_dimmer_cmd(instance, cmd, level, duration);
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

#endif

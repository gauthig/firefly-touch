/*
 * RV-C bus survey + probe (diagnostic build, issue #96). Everything here is
 * compiled out unless CONFIG_FIREFLY_BUS_SURVEY is set, so a production build
 * is unchanged. See docs/BUS-SURVEY.md.
 */
#pragma once

#include "sdkconfig.h"

#if CONFIG_FIREFLY_BUS_SURVEY

#include "driver/twai.h"
#include "esp_err.h"

/* Allocates the table and starts the reporter / command / probe tasks. */
esp_err_t bus_survey_start(void);

/* Called from twai_rx_task for every received frame. Non-blocking, never logs. */
void bus_survey_on_frame(const twai_message_t *msg);

#endif

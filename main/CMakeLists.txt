idf_component_register(
    SRCS "main.c" "twai_tasks.c" "state_manager.c" "bridge_tx.c" "ui/ui.c"
         "bus_survey_task.c"
    INCLUDE_DIRS "." "ui" "${CMAKE_SOURCE_DIR}/panels"
    REQUIRES rvc_protocol board ui_common espnow_link jbd_bms easytouch driver
             bus_survey esp_timer
)

# PANEL is the root-level cache variable (idf.py -DPANEL=<name>).
target_compile_definitions(${COMPONENT_LIB} PRIVATE PANEL_CONFIG_HEADER="${PANEL}.h")

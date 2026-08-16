idf_component_register(
    SRCS "main.c" "twai_tasks.c" "state_manager.c" "bridge_tx.c" "ui/ui.c"
    INCLUDE_DIRS "." "ui" "${CMAKE_SOURCE_DIR}/panels"
    REQUIRES rvc_protocol board_4_3b ui_common espnow_link driver
)

# PANEL is the root-level cache variable (idf.py -DPANEL=<name>).
target_compile_definitions(${COMPONENT_LIB} PRIVATE PANEL_CONFIG_HEADER="${PANEL}.h")

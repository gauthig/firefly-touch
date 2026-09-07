# easytouch_protocol.c is pure C (host-testable, host_test/); easytouch_client.c
# is the Bluedroid GATTC transport. REQUIRES is a constant list on purpose --
# nothing here is conditional on BOARD/PANEL (see the components/board note on
# IDF's early-expansion pass).
idf_component_register(
    SRCS "easytouch_protocol.c" "easytouch_client.c"
    INCLUDE_DIRS "include"
    REQUIRES ble_host bt
)

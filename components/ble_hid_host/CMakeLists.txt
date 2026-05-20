idf_component_register(
    SRCS "ble_hid_host.c"
    INCLUDE_DIRS "include"
    REQUIRES hid_defs bt esp_event storage
)

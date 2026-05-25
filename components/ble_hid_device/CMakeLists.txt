idf_component_register(
    SRCS "ble_hid_device.c"
    INCLUDE_DIRS "include"
    REQUIRES hid_defs ble_hid_host bt esp_event
)

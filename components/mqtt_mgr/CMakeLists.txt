idf_component_register(
    SRCS "mqtt_mgr.c"
    INCLUDE_DIRS "include"
    REQUIRES hid_defs storage usb_hid_device mqtt esp_event esp_wifi
)

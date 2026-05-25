idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES
        storage
        ble_hid_host
        usb_hid_device
        hotkey
        wifi_manager
        web_ui
        jiggler
        webhook
        mqtt_mgr
        nvs_flash
        esp_event
        driver
        esp_timer
)

idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES
        storage
        fido2
        ble_hid_host
        ble_hid_device
        usb_hid_device
        usb_hid_host
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

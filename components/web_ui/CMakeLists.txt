idf_component_register(
    SRCS "web_ui.c"
    INCLUDE_DIRS "include"
    REQUIRES storage hotkey jiggler wifi_manager ble_hid_host
             esp_http_server esp_http_client esp_wifi esp_system
             app_update mbedtls esp_app_format nvs_flash
             json freertos bootloader_support
    EMBED_TXTFILES "www/index.html"
)

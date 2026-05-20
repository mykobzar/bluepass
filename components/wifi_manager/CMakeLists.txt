idf_component_register(
    SRCS "wifi_manager.c"
    INCLUDE_DIRS "include"
    REQUIRES storage esp_wifi esp_event nvs_flash esp_netif lwip driver esp_timer
)

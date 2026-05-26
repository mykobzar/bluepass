idf_component_register(
    SRCS "fido2.c"
    INCLUDE_DIRS "include"
    REQUIRES storage nvs_flash mbedtls wifi_manager
    PRIV_REQUIRES esp_hw_support freertos
)

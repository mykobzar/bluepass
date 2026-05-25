idf_component_register(
    SRCS "webhook.c"
    INCLUDE_DIRS "include"
    REQUIRES hid_defs storage esp_http_client mbedtls
)

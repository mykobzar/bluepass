idf_component_register(
    SRCS "hotkey.c"
    INCLUDE_DIRS "include"
    REQUIRES hid_defs storage totp jiggler webhook mqtt_mgr
)

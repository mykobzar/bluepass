idf_component_register(
    SRCS "hotkey.c"
    INCLUDE_DIRS "include"
    REQUIRES hid_defs storage usb_hid_device totp jiggler
)

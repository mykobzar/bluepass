idf_component_register(
    SRCS "jiggler.c"
    INCLUDE_DIRS "include"
    REQUIRES usb_hid_device esp_timer
)

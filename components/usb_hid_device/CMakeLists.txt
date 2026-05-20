idf_component_register(
    SRCS "usb_hid_device.c"
    INCLUDE_DIRS "include"
    REQUIRES hid_defs espressif__esp_tinyusb
)

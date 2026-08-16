idf_build_get_property(python PYTHON)

set(WWW_SRC "${CMAKE_CURRENT_LIST_DIR}/www/index.html")
set(WWW_GZ  "${CMAKE_CURRENT_BINARY_DIR}/index.html.gz")

idf_component_register(
    SRCS "web_ui.c"
    INCLUDE_DIRS "include"
    REQUIRES storage hotkey jiggler wifi_manager ble_hid_host ble_hid_device usb_hid_host webhook mqtt_mgr fido2
             esp_http_server esp_http_client esp_wifi esp_system
             app_update esp_https_ota mbedtls esp-tls esp_app_format nvs_flash
             json freertos bootloader_support
    EMBED_FILES "${WWW_GZ}"
)

# The SPA is ~100 KB and esp_http_server runs a single thread: sending it raw
# monopolises the server for the whole transfer, which on a weak link is tens of
# seconds — every other request (and any button-driven httpd_stop) waits behind
# it. gzip cuts the page to roughly a quarter of that. mtime=0 keeps the
# compressed output byte-identical across rebuilds of an unchanged page.
add_custom_command(
    OUTPUT "${WWW_GZ}"
    COMMAND ${python} -c "import gzip,sys; open(sys.argv[2],'wb').write(gzip.compress(open(sys.argv[1],'rb').read(),9,mtime=0))" "${WWW_SRC}" "${WWW_GZ}"
    DEPENDS "${WWW_SRC}"
    COMMENT "Compressing web UI (index.html → index.html.gz)"
    VERBATIM
)
add_custom_target(web_ui_gzip DEPENDS "${WWW_GZ}")
add_dependencies(${COMPONENT_LIB} web_ui_gzip)

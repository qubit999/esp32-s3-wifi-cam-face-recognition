idf_component_register(SRCS "main.cpp" "face_recognition.cpp"
                       INCLUDE_DIRS "."
                       REQUIRES esp_psram esp_wifi esp_event nvs_flash esp_http_server esp_http_client esp32-camera led_strip driver esp_timer spiffs esp-tls mbedtls)

# Disable format warnings for ESP-DL compatibility
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-format)

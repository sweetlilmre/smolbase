idf_component_register(
  SRCS "main.cpp"
  INCLUDE_DIRS "."
  REQUIRES
    nvs_flash
    esp_wifi
    esp_netif
    esp_event
    esp_http_client
    esp_http_server
    esp-tls
    mbedtls
    esp_driver_touch_sens
    esp_timer
    app_update
    esp_app_format
    lovyangfx
)

target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu++17)

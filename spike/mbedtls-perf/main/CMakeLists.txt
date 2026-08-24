idf_component_register(
  SRCS "bench.c" "wrap.c"
  INCLUDE_DIRS "."
  REQUIRES mbedtls esp_timer
)

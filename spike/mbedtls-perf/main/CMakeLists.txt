idf_component_register(
  SRCS "bench.c" "wrap.c"
  INCLUDE_DIRS "."
  REQUIRES mbedtls esp_timer
)

# The crossover sweep calls mbedtls_mpi_core_mul() to time the software multiply
# in the same binary as the hardware one. That symbol is exported but declared in
# a library-private header, so the include path has to be added by hand -- the
# same problem espressif/esp-idf#19027 solves for the mbedtls component itself.
# IDF 6 only: the header moved under tf-psa-crypto in mbedTLS 4, and the
# crossover cells are all idf6-*.
# bignum_core.h pulls in constant_time_internal.h from a second private
# directory, and that name also exists inside openthread's vendored mbedTLS --
# so a missing path here does not fail as "not found" but as an ambiguity
# between two components, which is a confusing way to learn you need one more
# include directory.
if(SPIKE_CROSSOVER OR SPIKE_MULSTEP)
  target_include_directories(${COMPONENT_LIB} PRIVATE
    "$ENV{IDF_PATH}/components/mbedtls/mbedtls/tf-psa-crypto/drivers/builtin/src"
    "$ENV{IDF_PATH}/components/mbedtls/mbedtls/tf-psa-crypto/utilities")
endif()

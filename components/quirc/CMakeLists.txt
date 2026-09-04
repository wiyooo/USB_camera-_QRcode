idf_component_register(SRCS "lib/quirc.c" "lib/identify.c" "lib/decode.c" "lib/version_db.c"
                       INCLUDE_DIRS "lib")
# ESP32-S3 has a single-precision FPU. Keep the one-byte image/region alias.
target_compile_definitions(${COMPONENT_LIB} PRIVATE QUIRC_FLOAT_TYPE=float QUIRC_USE_TGMATH)

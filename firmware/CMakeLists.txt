# ESP-IDF project for the Cardputer ADV. The engine is a plain component: it has no
# IDF dependency, so the same sources build for the host (see tools/run_host_tests.sh).
idf_component_register(
    SRCS "main.c" "board_keys.c"
    INCLUDE_DIRS "." "../components/fmengine/include"
    REQUIRES driver esp_driver_i2s esp_driver_i2c esp_timer
)

# The portable engine, compiled into the app from its own directory so the host gates and
# the firmware are guaranteed to build the same files.
set(FMENGINE_DIR "${CMAKE_CURRENT_LIST_DIR}/../components/fmengine")
target_sources(${COMPONENT_LIB} PRIVATE
    "${FMENGINE_DIR}/src/fm_engine.c"
    "${FMENGINE_DIR}/src/fm_tables.c"
    "${FMENGINE_DIR}/src/fm_bank.c"
    "${FMENGINE_DIR}/src/fm_bank_data.c"
    "${FMENGINE_DIR}/src/fm_keys.c"
)

# The engine runs in the audio task, not in flash-cache-hostile code: keep it optimised.
target_compile_options(${COMPONENT_LIB} PRIVATE -O2)

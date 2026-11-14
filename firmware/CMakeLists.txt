# ESP-IDF app for the Cardputer ADV. The engine is the fmengine component; it has no IDF
# dependency, so the host gates build the same files (see tools/run_host_tests.sh).
idf_component_register(
    SRCS "main.c" "board_keys.c"
    INCLUDE_DIRS "."
    REQUIRES fmengine driver esp_driver_i2s esp_driver_i2c esp_timer
)

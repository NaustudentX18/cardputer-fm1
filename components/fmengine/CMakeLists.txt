# The engine as a real IDF component. It has no IDF dependency (only stdint/string), so the
# host gates compile exactly these files; this manifest exists so ESP-IDF registers the
# component instead of warning that it has no CMakeLists.
idf_component_register(
    SRCS
        "src/fm_engine.c"
        "src/fm_tables.c"
        "src/fm_bank.c"
        "src/fm_bank_data.c"
        "src/fm_keys.c"
        "src/fm_midi.c"
        "src/fm_ui.c"
        "src/fm_ui_font.c"
    INCLUDE_DIRS "include"
)

# Rendered inside the audio task: keep it optimised for speed, not size.
target_compile_options(${COMPONENT_LIB} PRIVATE -O2)

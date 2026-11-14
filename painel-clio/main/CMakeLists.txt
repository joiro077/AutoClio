idf_component_register(
    SRCS
        "main.c"
        "app/elm327.c"
        "app/obd_task.c"
        "app/app_state.c"
        "app/storage.c"
        "app/display.c"
        "app/ui.c"
        "app/font_big88.c"
        "app/logic_task.c"
        "core/settings.c"
        "core/elm_parse.c"
        "core/obd2_decode.c"
        "core/kwp_decode.c"
        "core/ecu_profiles_gen.c"
        "core/fuel.c"
        "core/trip.c"
        "core/gear.c"
        "core/injmon.c"
    INCLUDE_DIRS "." "core" "app"
    REQUIRES driver esp_lcd nvs_flash esp_timer
)

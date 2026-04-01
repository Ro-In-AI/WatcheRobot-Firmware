idf_component_register(
    SRCS "src/control_ingress.c"
    INCLUDE_DIRS "include"
    REQUIRES
        "behavior_state_service"
        "freertos"
        "hal_servo"
)

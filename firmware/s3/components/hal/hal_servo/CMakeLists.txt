idf_component_register(
    SRCS "src/hal_servo.c"
    INCLUDE_DIRS "include"
    # Phase 2: LEDC PWM on GPIO 19/20
    REQUIRES "driver"
    PRIV_REQUIRES "esp_timer"
)

idf_component_register(
    SRCS "src/hal_servo.c"
    INCLUDE_DIRS "include"
    # Phase 2: add "driver" for LEDC PWM
    REQUIRES "esp_err"
)

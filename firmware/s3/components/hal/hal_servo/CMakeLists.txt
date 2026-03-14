idf_component_register(
    SRCS "src/hal_servo.c"
    INCLUDE_DIRS "include"
    REQUIRES "driver"
    PRIV_REQUIRES "freertos"
)

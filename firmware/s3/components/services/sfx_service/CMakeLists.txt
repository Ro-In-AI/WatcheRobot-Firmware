idf_component_register(
    SRCS "src/sfx_service.c"
    INCLUDE_DIRS "include"
    REQUIRES
        "freertos"
        "hal_audio"
        "json"
    PRIV_REQUIRES
        "sensecap-watcher"
)

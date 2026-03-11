idf_component_register(
    SRCS
        "src/hal_audio.c"
        "src/hal_opus.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES
        "sensecap-watcher"
)

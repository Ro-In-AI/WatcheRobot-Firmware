idf_component_register(
    SRCS "src/hal_button.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES
        "sensecap-watcher"
)

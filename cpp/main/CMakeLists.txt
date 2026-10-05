idf_component_register(
    SRCS "main.cpp" "morse.cpp" "sh1106.cpp"
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_driver_gpio esp_driver_i2c esp_timer
)

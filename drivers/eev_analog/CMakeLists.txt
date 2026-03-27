idf_component_register(
    SRCS "src/eev_analog_driver.cpp"
    INCLUDE_DIRS "include"
    REQUIRES modesp_hal esp_driver_dac
)

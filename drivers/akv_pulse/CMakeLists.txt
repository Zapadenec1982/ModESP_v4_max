idf_component_register(
    SRCS "src/akv_pulse_driver.cpp"
    INCLUDE_DIRS "include"
    REQUIRES modesp_hal esp_driver_gpio
)

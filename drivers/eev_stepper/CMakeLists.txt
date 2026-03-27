idf_component_register(
    SRCS "src/eev_stepper_driver.cpp"
    INCLUDE_DIRS "include"
    REQUIRES modesp_hal esp_driver_gpio nvs_flash
)

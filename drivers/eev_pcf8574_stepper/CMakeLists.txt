idf_component_register(
    SRCS "src/eev_pcf8574_stepper_driver.cpp"
    INCLUDE_DIRS "include"
    REQUIRES modesp_hal nvs_flash
)

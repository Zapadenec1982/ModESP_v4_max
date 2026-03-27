idf_component_register(
    SRCS "src/pressure_adc_driver.cpp"
    INCLUDE_DIRS "include"
    REQUIRES modesp_hal
    PRIV_REQUIRES driver esp_adc
)

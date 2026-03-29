idf_component_register(
    SRCS "src/modbus_master_service.cpp"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES
        modesp_core
        modesp_services
        driver
)

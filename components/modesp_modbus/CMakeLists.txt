idf_component_register(
    SRCS "src/modbus_service.cpp"
    INCLUDE_DIRS "include"
    REQUIRES modesp_core esp_http_server
    PRIV_REQUIRES nvs_flash driver modesp_services
)
target_include_directories(${COMPONENT_LIB} PRIVATE "${CMAKE_SOURCE_DIR}/generated")

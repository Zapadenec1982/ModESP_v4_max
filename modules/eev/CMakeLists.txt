idf_component_register(
    SRCS "src/eev_module.cpp"
    INCLUDE_DIRS "include"
    REQUIRES modesp_core modesp_refrigerant
)
target_include_directories(${COMPONENT_LIB} PRIVATE "${CMAKE_SOURCE_DIR}/generated")

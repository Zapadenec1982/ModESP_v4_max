# Protection module — alarm monitoring and protection
# No HAL dependency — works through SharedState (Equipment Manager)
idf_component_register(
    SRCS "src/protection_module.cpp"
    INCLUDE_DIRS "include"
    REQUIRES modesp_core
)

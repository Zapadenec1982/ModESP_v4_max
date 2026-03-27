# Thermostat business logic module
# No HAL dependency — works through SharedState (Equipment Manager)
idf_component_register(
    SRCS "src/thermostat_module.cpp"
    INCLUDE_DIRS "include"
    REQUIRES modesp_core
)

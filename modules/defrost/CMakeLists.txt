# Defrost cycle module — 7-phase state machine
# No HAL dependency — works through SharedState (Equipment Manager)
idf_component_register(
    SRCS "src/defrost_module.cpp"
    INCLUDE_DIRS "include"
    REQUIRES modesp_core
)

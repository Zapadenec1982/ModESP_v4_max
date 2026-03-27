# jsmn — lightweight JSON parser (header-only, public domain)
idf_component_register(INCLUDE_DIRS ".")
target_compile_definitions(${COMPONENT_LIB} INTERFACE JSMN_STATIC)

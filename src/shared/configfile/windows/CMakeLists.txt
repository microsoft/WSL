add_library(configfile STATIC ${SOURCES} ${HEADERS})
add_dependencies(configfile wslserviceidl localization common)
set_target_properties(configfile PROPERTIES FOLDER common)
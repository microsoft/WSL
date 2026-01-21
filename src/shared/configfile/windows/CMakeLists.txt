add_library(configfile STATIC ${SOURCES} ${HEADERS})
add_dependencies(configfile wslserviceidl localization)
set_target_properties(configfile PROPERTIES FOLDER common)
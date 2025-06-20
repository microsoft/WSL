add_linux_library_no_sources(libconfigfile "${SOURCES}" "${HEADERS}")
set_target_properties(libconfigfile PROPERTIES FOLDER common)
add_dependencies(libconfigfile localization)
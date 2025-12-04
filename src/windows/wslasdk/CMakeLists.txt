set(SOURCES wsla.cpp)
set(HEADERS wsla.h)

add_library(wslasdk SHARED ${SOURCES} ${HEADERS} wslasdk.def)
set_target_properties(wslasdk PROPERTIES EXCLUDE_FROM_ALL FALSE)
add_dependencies(wslasdk wslaserviceidl)
target_link_libraries(wslasdk ${COMMON_LINK_LIBRARIES} legacy_stdio_definitions common)
target_precompile_headers(wslasdk REUSE_FROM common)
set_target_properties(wslasdk PROPERTIES FOLDER windows)
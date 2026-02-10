set(SOURCES 
    wslcsdk.cpp
    WslcsdkPrivate.cpp
set(HEADERS
    wslcsdk.h
    WslcsdkPrivate.h
)

add_library(wslcsdk SHARED ${SOURCES} ${HEADERS} wslcsdk.def)
set_target_properties(wslcsdk PROPERTIES EXCLUDE_FROM_ALL FALSE)
add_dependencies(wslcsdk wslaserviceidl)
target_link_libraries(wslcsdk ${COMMON_LINK_LIBRARIES} legacy_stdio_definitions common)
target_precompile_headers(wslcsdk REUSE_FROM common)
set_target_properties(wslcsdk PROPERTIES FOLDER windows)

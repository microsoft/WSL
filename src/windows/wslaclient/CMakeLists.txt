set(SOURCES DllMain.cpp)
set(HEADERS WSLAApi.h)

add_library(wslaclient SHARED ${SOURCES} ${HEADERS} wslaclient.def)
set_target_properties(wslaclient PROPERTIES EXCLUDE_FROM_ALL FALSE)
target_link_libraries(wslaclient ${COMMON_LINK_LIBRARIES} legacy_stdio_definitions common)
target_precompile_headers(wslaclient REUSE_FROM common)
set_target_properties(wslaclient PROPERTIES FOLDER windows)
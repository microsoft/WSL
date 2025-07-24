set(SOURCES DllMain.cpp)
set(HEADERS LSWApi.h)

add_library(lswclient SHARED ${SOURCES} ${HEADERS} lswclient.def)
set_target_properties(lswclient PROPERTIES EXCLUDE_FROM_ALL FALSE)
target_link_libraries(lswclient ${COMMON_LINK_LIBRARIES} legacy_stdio_definitions common)
target_precompile_headers(lswclient REUSE_FROM common)
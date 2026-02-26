set(WSLC_SUBDIRS arguments commands core services tasks)
list(TRANSFORM WSLC_SUBDIRS PREPEND ${CMAKE_CURRENT_SOURCE_DIR}/ OUTPUT_VARIABLE WSLC_SUBDIR_PATHS)

list(TRANSFORM WSLC_SUBDIR_PATHS APPEND /*.h OUTPUT_VARIABLE HEADER_PATTERNS)
list(TRANSFORM WSLC_SUBDIR_PATHS APPEND /*.cpp OUTPUT_VARIABLE SOURCE_PATTERNS)
file(GLOB_RECURSE HEADERS CONFIGURE_DEPENDS ${HEADER_PATTERNS})
file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS ${SOURCE_PATTERNS})

# Object library for WSLC components.
# Used to build the executable and also unit testing components.
add_library(wslclib OBJECT ${SOURCES} ${HEADERS})
target_include_directories(wslclib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${WSLC_SUBDIR_PATHS})

target_link_libraries(wslclib
                      ${COMMON_LINK_LIBRARIES}
                      common)

target_precompile_headers(wslclib REUSE_FROM common)
set_target_properties(wslclib PROPERTIES FOLDER windows)

# Create wslc.exe
add_executable(wslc $<TARGET_OBJECTS:wslclib>)

target_link_libraries(wslc wslclib)

set_target_properties(wslc PROPERTIES FOLDER windows)

# For prettier source tree browsing
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${SOURCES} ${HEADERS})

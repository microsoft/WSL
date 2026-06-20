set(WSLC_SUBDIRS arguments commands core services tasks)
list(TRANSFORM WSLC_SUBDIRS PREPEND ${CMAKE_CURRENT_SOURCE_DIR}/ OUTPUT_VARIABLE WSLC_SUBDIR_PATHS)

list(TRANSFORM WSLC_SUBDIR_PATHS APPEND /*.h OUTPUT_VARIABLE HEADER_PATTERNS)
list(TRANSFORM WSLC_SUBDIR_PATHS APPEND /*.cpp OUTPUT_VARIABLE SOURCE_PATTERNS)
file(GLOB_RECURSE HEADERS CONFIGURE_DEPENDS ${HEADER_PATTERNS})
file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS ${SOURCE_PATTERNS})

set(WSLC_MAIN ${CMAKE_CURRENT_SOURCE_DIR}/core/Main.cpp)
list(REMOVE_ITEM SOURCES ${WSLC_MAIN})

# Object library for WSLC components.
# Used by the wslc executable, unit tests, and fuzzing harnesses.
add_library(wslclib OBJECT ${SOURCES} ${HEADERS})
target_include_directories(wslclib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${WSLC_SUBDIR_PATHS})

target_link_libraries(wslclib
                      ${COMMON_LINK_LIBRARIES}
                      yaml-cpp
                      common
                      advapi32
                      crypt32)

target_precompile_headers(wslclib REUSE_FROM common)
set_target_properties(wslclib PROPERTIES FOLDER windows)

# Create wslc.exe
# N.B. Linking wslclib (OBJECT library) brings both object files and
# transitive dependencies. Do not also use $<TARGET_OBJECTS:wslclib>.
add_executable(wslc ${WSLC_MAIN})

target_link_libraries(wslc wslclib)
target_precompile_headers(wslc REUSE_FROM common)

set_target_properties(wslc PROPERTIES FOLDER windows)

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${SOURCES} ${HEADERS})

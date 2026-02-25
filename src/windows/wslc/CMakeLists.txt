include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/core
    ${CMAKE_CURRENT_SOURCE_DIR}/commands
    ${CMAKE_CURRENT_SOURCE_DIR}/arguments
    ${CMAKE_CURRENT_SOURCE_DIR}/services
    ${CMAKE_CURRENT_SOURCE_DIR}/tasks
)

file(GLOB_RECURSE HEADERS CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/core/*.h
    ${CMAKE_CURRENT_SOURCE_DIR}/arguments/*.h
    ${CMAKE_CURRENT_SOURCE_DIR}/commands/*.h
    ${CMAKE_CURRENT_SOURCE_DIR}/services/*.h
    ${CMAKE_CURRENT_SOURCE_DIR}/tasks/*.h
)

file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/core/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/arguments/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/commands/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/services/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/tasks/*.cpp
)

# Object library for WSLC components.
# Used to build the executable and also unit testing components.
add_library(wslclib OBJECT ${SOURCES} ${HEADERS})
set_target_properties(wslclib PROPERTIES
# TODO: UNITY builds for wslclib are currently disabled because they have not been
#       validated for build performance and diagnostics in this project. Re-enable
#       the following properties once UNITY builds have been evaluated and approved:
#    UNITY_BUILD ON
#    UNITY_BUILD_BATCH_SIZE 0  # 0 means CMake decides automatically
    FOLDER windows
)
target_include_directories(wslclib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/core
    ${CMAKE_CURRENT_SOURCE_DIR}/commands
    ${CMAKE_CURRENT_SOURCE_DIR}/arguments
    ${CMAKE_CURRENT_SOURCE_DIR}/services
    ${CMAKE_CURRENT_SOURCE_DIR}/tasks
)
target_precompile_headers(wslclib REUSE_FROM common)
target_link_libraries(wslclib
    PRIVATE
    ${COMMON_LINK_LIBRARIES}
    common
)

# Create wslc.exe
add_executable(wslc $<TARGET_OBJECTS:wslclib>)
target_link_libraries(wslc
    PRIVATE
    ${COMMON_LINK_LIBRARIES}
    common
)
set_target_properties(wslc PROPERTIES FOLDER windows)

# For prettier source tree browsing
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${SOURCES} ${HEADERS})

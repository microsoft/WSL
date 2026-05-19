set(WSLC_SUBDIRS arguments commands core services tasks)
list(TRANSFORM WSLC_SUBDIRS PREPEND ${CMAKE_CURRENT_SOURCE_DIR}/ OUTPUT_VARIABLE WSLC_SUBDIR_PATHS)

list(TRANSFORM WSLC_SUBDIR_PATHS APPEND /*.h OUTPUT_VARIABLE HEADER_PATTERNS)
list(TRANSFORM WSLC_SUBDIR_PATHS APPEND /*.cpp OUTPUT_VARIABLE SOURCE_PATTERNS)
file(GLOB_RECURSE HEADERS CONFIGURE_DEPENDS ${HEADER_PATTERNS})
file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS ${SOURCE_PATTERNS})

set(WSLC_ADDITIONAL_INCLUDE_DIRS)
set(WSLC_ADDITIONAL_LIBRARIES)

if (INCLUDE_OPENVMM)
    set(WSLC_PROTO_OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
    wsl_generate_openvmm_proto(${WSLC_PROTO_OUTPUT_DIR} WSLC_GENERATED_PROTO_SRC WSLC_GENERATED_PROTO_HDR)

    list(APPEND WSLC_ADDITIONAL_INCLUDE_DIRS ${WSLC_PROTO_OUTPUT_DIR})
    list(APPEND WSLC_ADDITIONAL_LIBRARIES ${PROTOBUF_LIB_TARGET})

    if (MSVC)
        set_source_files_properties(${WSLC_GENERATED_PROTO_SRC} PROPERTIES COMPILE_OPTIONS "/wd4267;/wd4244;/wd4018")
    endif()
endif()

# Object library for WSLC components.
# Used to build the executable and also unit testing components.
add_library(wslclib OBJECT ${SOURCES} ${HEADERS})
target_include_directories(wslclib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${WSLC_SUBDIR_PATHS})
if (WSLC_ADDITIONAL_INCLUDE_DIRS)
    target_include_directories(wslclib PUBLIC ${WSLC_ADDITIONAL_INCLUDE_DIRS})
endif()
if (INCLUDE_OPENVMM)
    target_sources(wslclib PRIVATE ${WSLC_GENERATED_PROTO_SRC} ${WSLC_GENERATED_PROTO_HDR})
endif()

target_link_libraries(wslclib
                      ${COMMON_LINK_LIBRARIES}
                      ${WSLC_ADDITIONAL_LIBRARIES}
                      yaml-cpp
                      common
                      advapi32
                      crypt32)


target_precompile_headers(wslclib REUSE_FROM common)
set_target_properties(wslclib PROPERTIES FOLDER windows)

# Create wslc.exe
# N.B. Linking wslclib (OBJECT library) brings both object files and
# transitive dependencies. Do not also use $<TARGET_OBJECTS:wslclib>.
add_executable(wslc)

target_link_libraries(wslc wslclib)

set_target_properties(wslc PROPERTIES FOLDER windows)

# For prettier source tree browsing
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${SOURCES} ${HEADERS})

set(WSLC_SUBDIRS arguments commands core services tasks)
list(TRANSFORM WSLC_SUBDIRS PREPEND ${CMAKE_CURRENT_SOURCE_DIR}/ OUTPUT_VARIABLE WSLC_SUBDIR_PATHS)

list(TRANSFORM WSLC_SUBDIR_PATHS APPEND /*.h OUTPUT_VARIABLE HEADER_PATTERNS)
list(TRANSFORM WSLC_SUBDIR_PATHS APPEND /*.cpp OUTPUT_VARIABLE SOURCE_PATTERNS)
file(GLOB_RECURSE HEADERS CONFIGURE_DEPENDS ${HEADER_PATTERNS})
file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS ${SOURCE_PATTERNS})

set(WSLC_PROTO_FILE ${WSL_DEVICE_HOST_SOURCE_DIR}/proto/VMService.proto)
set(WSLC_PROTO_OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
set(WSLC_GENERATED_PROTO_SRC ${WSLC_PROTO_OUTPUT_DIR}/VMService.pb.cc)
set(WSLC_GENERATED_PROTO_HDR ${WSLC_PROTO_OUTPUT_DIR}/VMService.pb.h)

if (NOT EXISTS ${WSLC_PROTO_FILE})
    message(FATAL_ERROR "Expected DeviceHost proto file was not found: ${WSLC_PROTO_FILE}")
endif()

add_custom_command(
    OUTPUT ${WSLC_GENERATED_PROTO_SRC} ${WSLC_GENERATED_PROTO_HDR}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${WSLC_PROTO_OUTPUT_DIR}
    COMMAND $<TARGET_FILE:${PROTOC_TARGET}>
            --cpp_out=${WSLC_PROTO_OUTPUT_DIR}
            --proto_path=${WSL_DEVICE_HOST_SOURCE_DIR}/proto
            --proto_path=${protobuf_SOURCE_DIR}/src
            ${WSLC_PROTO_FILE}
    DEPENDS ${WSLC_PROTO_FILE} ${PROTOC_TARGET}
    VERBATIM)

if (MSVC)
    set_source_files_properties(${WSLC_GENERATED_PROTO_SRC} PROPERTIES COMPILE_OPTIONS "/wd4267;/wd4244;/wd4018")
endif()

# Object library for WSLC components.
# Used to build the executable and also unit testing components.
add_library(wslclib OBJECT ${SOURCES} ${HEADERS})
target_include_directories(wslclib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${WSLC_SUBDIR_PATHS})
target_include_directories(wslclib PUBLIC ${WSLC_PROTO_OUTPUT_DIR})
target_sources(wslclib PRIVATE ${WSLC_GENERATED_PROTO_SRC} ${WSLC_GENERATED_PROTO_HDR})

target_link_libraries(wslclib
                      ${COMMON_LINK_LIBRARIES}
                      ${PROTOBUF_LIB_TARGET}
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

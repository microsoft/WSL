set(WSLC_SUBDIRS arguments commands core services tasks)
list(TRANSFORM WSLC_SUBDIRS PREPEND ${CMAKE_CURRENT_SOURCE_DIR}/ OUTPUT_VARIABLE WSLC_SUBDIR_PATHS)

list(TRANSFORM WSLC_SUBDIR_PATHS APPEND /*.h OUTPUT_VARIABLE HEADER_PATTERNS)
list(TRANSFORM WSLC_SUBDIR_PATHS APPEND /*.cpp OUTPUT_VARIABLE SOURCE_PATTERNS)
file(GLOB_RECURSE HEADERS CONFIGURE_DEPENDS ${HEADER_PATTERNS})
file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS ${SOURCE_PATTERNS})

# Build Go template renderer library
set(GO_TEMPLATE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/tools/gotemplate)
set(GO_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/gotemplate")

# Reuse TARGET_PLATFORM already normalized by the root CMakeLists.txt
if("${TARGET_PLATFORM}" STREQUAL "arm64")
    set(GO_ARCH arm64)
    set(GO_LIB_MACHINE ARM64)
else()
    set(GO_ARCH amd64)
    set(GO_LIB_MACHINE X64)
endif()

set(GO_OUTPUT_DLL ${GO_OUTPUT_DIR}/render.dll)
set(GO_OUTPUT_LIB ${GO_OUTPUT_DIR}/render.lib)
add_custom_command(
    OUTPUT ${GO_OUTPUT_DLL} ${GO_OUTPUT_LIB}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${GO_OUTPUT_DIR}
    COMMAND ${CMAKE_COMMAND} -E env GOOS=windows GOARCH=${GO_ARCH} CGO_ENABLED=1
            go build -o ${GO_OUTPUT_DLL} -buildmode=c-shared -trimpath -ldflags=-s\ -w render.go
    COMMAND lib /def:${GO_TEMPLATE_DIR}/render.def /out:${GO_OUTPUT_LIB} /machine:${GO_LIB_MACHINE}
    WORKING_DIRECTORY ${GO_TEMPLATE_DIR}
    DEPENDS ${GO_TEMPLATE_DIR}/render.go ${GO_TEMPLATE_DIR}/render.def
    COMMENT "Building Go template renderer library"
    VERBATIM
)

add_custom_target(gotemplate_lib ALL DEPENDS ${GO_OUTPUT_DLL} ${GO_OUTPUT_LIB})

# Include Go source files for browsing in Solution Explorer
set(GO_SOURCES ${GO_TEMPLATE_DIR}/render.go)
set_source_files_properties(${GO_SOURCES} PROPERTIES HEADER_FILE_ONLY TRUE)

# Object library for WSLC components.
# Used to build the executable and also unit testing components.
add_library(wslclib OBJECT ${SOURCES} ${HEADERS} ${GO_SOURCES})
target_include_directories(wslclib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${WSLC_SUBDIR_PATHS})

target_link_libraries(wslclib
                      ${COMMON_LINK_LIBRARIES}
                      yaml-cpp
                      common
                      ${GO_OUTPUT_LIB})

# Add dependency on Go library
add_dependencies(wslclib gotemplate_lib)

target_precompile_headers(wslclib REUSE_FROM common)
set_target_properties(wslclib PROPERTIES FOLDER windows)

# Create wslc.exe
add_executable(wslc $<TARGET_OBJECTS:wslclib>)

target_link_libraries(wslc wslclib)

add_custom_command(TARGET wslc POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${GO_OUTPUT_DLL} $<TARGET_FILE_DIR:wslc>/render.dll
)

set_target_properties(wslc PROPERTIES FOLDER windows)

# For prettier source tree browsing
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${SOURCES} ${HEADERS} ${GO_SOURCES})

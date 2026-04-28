add_idl_winrt(wslcsdkwinrtidl "wslcsdk.idl")
set_target_properties(wslcsdkwinrtidl PROPERTIES FOLDER windows)

set(SOURCES
    Session.cpp
    SessionSettings.cpp
    VhdRequirements.cpp
)

set(HEADERS
    Helpers.h
    Session.h
    SessionSettings.h
    VhdRequirements.h
)

add_library(wslcsdkwinrt STATIC ${SOURCES} ${HEADERS})
add_dependencies(wslcsdkwinrt wslcsdkwinrtidl)

set(MODULE_G_CPP ${CMAKE_CURRENT_BINARY_DIR}/${TARGET_PLATFORM}/${CMAKE_BUILD_TYPE}/module.g.cpp)
target_sources(wslcsdkwinrt PRIVATE ${MODULE_G_CPP})
set_source_files_properties(
    ${MODULE_G_CPP}
    PROPERTIES
    GENERATED TRUE)

target_precompile_headers(wslcsdkwinrt PRIVATE precomp.h)
target_include_directories(wslcsdkwinrt PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}/${TARGET_PLATFORM}/${CMAKE_BUILD_TYPE}
    ${CMAKE_SOURCE_DIR}/src/windows/WslcSDK
)

set_target_properties(
    wslcsdkwinrt
    PROPERTIES
    FOLDER windows
    EXCLUDE_FROM_ALL FALSE
)

set(SOURCES
    IOCallback.cpp
    ProgressCallback.cpp
    TerminationCallback.cpp
    wslcsdk.cpp
    WslcsdkPrivate.cpp
)
set(HEADERS
    Defaults.h
    IOCallback.h
    ProgressCallback.h
    TerminationCallback.h
    wslcsdk.h
    WslcsdkPrivate.h
)

add_library(wslcsdk SHARED ${SOURCES} ${HEADERS} wslcsdk.def)
set_target_properties(wslcsdk
    PROPERTIES
    EXCLUDE_FROM_ALL FALSE
    FOLDER windows
)

add_subdirectory(winrt)

if (WSL_BUILD_SDKCS)
    add_subdirectory(csharp)
endif()

add_dependencies(wslcsdk wslserviceidl wslcsdkwinrt)
target_link_libraries(wslcsdk ${COMMON_LINK_LIBRARIES} legacy_stdio_definitions common wslcsdkwinrt)
target_precompile_headers(wslcsdk REUSE_FROM common)
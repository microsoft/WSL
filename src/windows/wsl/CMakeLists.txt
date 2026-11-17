set(SOURCES
    main.cpp
    main.rc
    # Keep Authz's 16-bit message table separate from the service's customer-bit messages.
    ${CMAKE_CURRENT_BINARY_DIR}/../service/mc/${TARGET_PLATFORM}/${CMAKE_BUILD_TYPE}/wslsecurityaudit.rc)

set(HEADERS
    resource.h)

add_executable(wsl ${SOURCES} ${HEADERS})
add_dependencies(wsl wslsecurityauditmc)

target_link_libraries(wsl
                      ${COMMON_LINK_LIBRARIES}
                      ${MSI_LINK_LIBRARIES}
                      common
                      runtimeobject.lib
                      delayimp.lib)

set_target_properties(wsl PROPERTIES LINK_FLAGS "/DELAYLOAD:msi.dll /DELAYLOAD:WINTRUST.dll")
target_precompile_headers(wsl REUSE_FROM common)
set_target_properties(wsl PROPERTIES FOLDER windows)
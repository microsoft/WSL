set(SOURCES
    ServiceMain.cpp
    WslInstallerFactory.cpp
    WslInstaller.cpp
    main.rc)

set (HEADERS
     WslInstallerFactory.h
     WslInstaller.h
     ServiceUpgradeGuard.h)

include_directories(${CMAKE_BINARY_DIR}/src/windows/wslinstaller/inc/${TARGET_PLATFORM}/${CMAKE_BUILD_TYPE})

add_executable(wslinstaller ${SOURCES} ${HEADERS})
option(WSL_EXPERIMENTAL_UPGRADE_GUARD "Test legacy upgrade service activation guard (not production ready)" OFF)
if(WSL_EXPERIMENTAL_UPGRADE_GUARD)
    target_compile_definitions(wslinstaller PRIVATE WSL_EXPERIMENTAL_UPGRADE_GUARD)
endif()
add_dependencies(wslinstaller wslinstalleridl)

set_target_properties(wslinstaller PROPERTIES LINK_FLAGS "/merge:minATL=.rdata /include:__minATLObjMap_WslInstaller_COM")
target_link_libraries(wslinstaller
                      ${COMMON_LINK_LIBRARIES}
                      ${MSI_LINK_LIBRARIES}
                      common
                      legacy_stdio_definitions)

target_precompile_headers(wslinstaller REUSE_FROM common)
set_target_properties(wslinstaller PROPERTIES FOLDER windows)

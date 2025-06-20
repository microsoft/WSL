set(SOURCES
    ServiceMain.cpp
    WslInstallerFactory.cpp
    WslInstaller.cpp
    main.rc)

set (HEADERS
     WslInstallerFactory.h
     WslInstaller.h)

include_directories(${CMAKE_BINARY_DIR}/src/windows/wslinstaller/inc/${TARGET_PLATFORM}/${CMAKE_BUILD_TYPE})

add_executable(wslinstaller ${SOURCES} ${HEADERS})
add_dependencies(wslinstaller wslinstalleridl)

set_target_properties(wslinstaller PROPERTIES LINK_FLAGS "/merge:minATL=.rdata /include:__minATLObjMap_WslInstaller_COM")
target_link_libraries(wslinstaller
                      ${COMMON_LINK_LIBRARIES}
                      common
                      legacy_stdio_definitions)

target_precompile_headers(wslinstaller REUSE_FROM common)
set_target_properties(wslinstaller PROPERTIES FOLDER windows)
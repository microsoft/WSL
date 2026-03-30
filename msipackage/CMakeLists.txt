set(PACKAGE_WIX_IN ${CMAKE_CURRENT_LIST_DIR}/package.wix.in)
set(WINDOWS_BINARIES wsl.exe;wslg.exe;wslhost.exe;wslrelay.exe;wslservice.exe;wslserviceproxystub.dll;wslinstall.dll)
if (WSL_BUILD_WSL_SETTINGS)
    list(APPEND WINDOWS_BINARIES "wslsettings/wslsettings.dll;wslsettings/wslsettings.exe;libwsl.dll")
endif()

set(LINUX_BINARIES init)

if (${WSL_BUILD_THIN_PACKAGE})
    set(COMPRESS_PACKAGE "no")
else()
    set(COMPRESS_PACKAGE "yes")
endif()

# Save whether PACKAGE_INPUT_DIR was externally set (pipeline builds)
if(DEFINED PACKAGE_INPUT_DIR)
    set(_EXTERNAL_PACKAGE_INPUT_DIR "${PACKAGE_INPUT_DIR}")
endif()

# Generate .wix file for each configuration since configure_file can't use generator expressions
foreach(config Debug Release)
    set(CONFIG_BIN ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${config})

    if(DEFINED _EXTERNAL_PACKAGE_INPUT_DIR)
        set(PACKAGE_INPUT_DIR ${_EXTERNAL_PACKAGE_INPUT_DIR})
    else()
        set(PACKAGE_INPUT_DIR ${CONFIG_BIN})
    endif()

    # Variables referenced by package.wix.in
    set(BIN ${CONFIG_BIN})
    set(PACKAGE_WIX ${CONFIG_BIN}/package.wix)
    set(CAB_CACHE ${CONFIG_BIN}/cab)
    file(MAKE_DIRECTORY ${CONFIG_BIN})
    file(MAKE_DIRECTORY ${CAB_CACHE})
    configure_file(${PACKAGE_WIX_IN} ${PACKAGE_WIX})
endforeach()

# Use $<CONFIG> for build-time paths in custom commands
set(BIN_CFG ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/$<CONFIG>)
set(OUTPUT_PACKAGE ${BIN_CFG}/wsl.msi)
set(PACKAGE_WIX ${BIN_CFG}/package.wix)
set(CAB_CACHE ${BIN_CFG}/cab)
set(COMPRESSION "$<IF:$<CONFIG:Debug>,none,high>")

set(BINARIES_DEPENDENCIES)
foreach(binary ${WINDOWS_BINARIES})
    list(APPEND BINARIES_DEPENDENCIES "${BIN_CFG}/${binary}")
endforeach()
foreach(binary ${LINUX_BINARIES})
    list(APPEND BINARIES_DEPENDENCIES "${BIN_CFG}/${binary}")
endforeach()

add_custom_command(
    OUTPUT ${OUTPUT_PACKAGE}
    COMMAND ${WIX_SOURCE_DIR}/wix.exe build ${PACKAGE_WIX} -o ${OUTPUT_PACKAGE} -arch ${TARGET_PLATFORM} -dcl ${COMPRESSION} -cc ${CAB_CACHE} -pdbtype none
    COMMAND ${CMAKE_COMMAND} -E touch "${CMAKE_CURRENT_BINARY_DIR}/CmakeFiles/msipackage"
    VERBATIM
    DEPENDS ${PACKAGE_WIX} ${BINARIES_DEPENDENCIES} # Make sure the package is rebuilt if any of the binaries or resources change
)

add_custom_target(msipackage DEPENDS ${OUTPUT_PACKAGE})
set_target_properties(msipackage PROPERTIES EXCLUDE_FROM_ALL FALSE SOURCES ${PACKAGE_WIX_IN})
add_dependencies(msipackage wsl wslg wslservice wslhost wslrelay wslserviceproxystub init wslinstall msixgluepackage)

if (WSL_BUILD_WSL_SETTINGS)
    add_dependencies(msipackage wslsettings libwsl)
endif()

set_source_files_properties(${OUTPUT_PACKAGE} PROPERTIES GENERATED TRUE)

if (DEFINED WSL_POST_BUILD_COMMAND)
    add_custom_command(
        TARGET msipackage
        POST_BUILD
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        USES_TERMINAL
        COMMAND ${WSL_POST_BUILD_COMMAND} -Platform ${TARGET_PLATFORM} -BuildType $<CONFIG>)
endif()
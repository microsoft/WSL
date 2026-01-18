set(BIN ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${CMAKE_BUILD_TYPE})

# For pipeline builds, use PACKAGE_INPUT_DIR if specified (contains signed binaries)
# For local builds, use bin directory
if(DEFINED PACKAGE_INPUT_DIR)
    message(STATUS "Using signed binaries from ${PACKAGE_INPUT_DIR} for MSI packaging")
else()
    set(PACKAGE_INPUT_DIR ${BIN})
endif()

set(OUTPUT_PACKAGE ${BIN}/wsl.msi)
set(PACKAGE_WIX_IN ${CMAKE_CURRENT_LIST_DIR}/package.wix.in)
set(PACKAGE_WIX ${BIN}/package.wix)
set(CAB_CACHE ${BIN}/cab)
set(WINDOWS_BINARIES wsl.exe;wslg.exe;wslhost.exe;wslrelay.exe;wslservice.exe;wslserviceproxystub.dll;wslinstall.dll)
if (WSL_BUILD_WSL_SETTINGS)
    list(APPEND WINDOWS_BINARIES "wslsettings/wslsettings.dll;wslsettings/wslsettings.exe;libwsl.dll")
endif()

set(BINARIES_DEPENDENCIES)
foreach(binary ${WINDOWS_BINARIES})
    list(APPEND BINARIES_DEPENDENCIES "${PACKAGE_INPUT_DIR}/${binary}")
endforeach()

set(LINUX_BINARIES init;initrd.img)
foreach(binary ${LINUX_BINARIES})
    list(APPEND BINARIES_DEPENDENCIES "${BIN}/${binary}")
endforeach()

if (${WSL_BUILD_THIN_PACKAGE})
    set(COMPRESS_PACKAGE "no")
else()
    set(COMPRESS_PACKAGE "yes")
endif()

configure_file(${PACKAGE_WIX_IN} ${PACKAGE_WIX})
file(MAKE_DIRECTORY ${CAB_CACHE})

if (NOT ${CMAKE_BUILD_TYPE} STREQUAL "Debug")
    set(COMPRESSION "high")
else()
    set(COMPRESSION "none")
endif()

add_custom_command(
    OUTPUT ${OUTPUT_PACKAGE}
    COMMAND ${WIX_SOURCE_DIR}/wix.exe build ${PACKAGE_WIX} -o ${OUTPUT_PACKAGE} -arch ${TARGET_PLATFORM} -dcl ${COMPRESSION} -cc ${CAB_CACHE} -pdbtype none
    COMMAND ${CMAKE_COMMAND} -E touch "${CMAKE_CURRENT_BINARY_DIR}/CmakeFiles/msipackage"
    VERBATIM
    DEPENDS ${PACKAGE_WIX} ${BINARIES_DEPENDENCIES} # Make sure the package is rebuilt if any of the binaries or resources change
)

add_custom_target(msipackage DEPENDS ${OUTPUT_PACKAGE})
set_target_properties(msipackage PROPERTIES EXCLUDE_FROM_ALL FALSE SOURCES ${PACKAGE_WIX_IN})
add_dependencies(msipackage wsl wslg wslservice wslhost wslrelay wslserviceproxystub init initramfs wslinstall msixgluepackage)

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
        COMMAND ${WSL_POST_BUILD_COMMAND} -Platform ${TARGET_PLATFORM} -BuildType ${CMAKE_BUILD_TYPE})
endif()
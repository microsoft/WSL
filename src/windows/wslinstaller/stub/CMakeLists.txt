add_compile_definitions("PROXY_CLSID_IS={ 0xce1044f6, 0x36c5, 0x4599, { 0xa5, 0xa8, 0x3b, 0xbf, 0x27, 0xba, 0x54, 0x95 } }")
add_compile_definitions("REGISTER_PROXY_DLL")

set(SOURCES
    ${CMAKE_CURRENT_BINARY_DIR}/../inc/${TARGET_PLATFORM}/${CMAKE_BUILD_TYPE}/wslinstallerservice_i_${TARGET_PLATFORM}.c
    ${CMAKE_CURRENT_BINARY_DIR}/../inc/${TARGET_PLATFORM}/${CMAKE_BUILD_TYPE}/wslinstallerservice_p_${TARGET_PLATFORM}.c
    ${CMAKE_CURRENT_BINARY_DIR}/../inc/${TARGET_PLATFORM}/${CMAKE_BUILD_TYPE}/dlldata_${TARGET_PLATFORM}.c
    ${CMAKE_CURRENT_LIST_DIR}/WslInstallerProxyStub.def
    ${CMAKE_CURRENT_LIST_DIR}/WslInstallerProxyStub.rc)

set_source_files_properties(${SOURCES} PROPERTIES GENERATED TRUE)

add_library(wslinstallerproxystub SHARED ${SOURCES})
add_dependencies(wslinstallerproxystub wslinstalleridl)
target_link_libraries(wslinstallerproxystub ${COMMON_LINK_LIBRARIES})
set_target_properties(wslinstallerproxystub PROPERTIES FOLDER windows)

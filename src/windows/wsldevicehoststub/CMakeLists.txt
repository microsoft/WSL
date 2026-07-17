set(SOURCES
    ${CMAKE_CURRENT_BINARY_DIR}/inc/${TARGET_PLATFORM}/${CMAKE_BUILD_TYPE}/WslDeviceHost_i_${TARGET_PLATFORM}.c
    ${CMAKE_CURRENT_BINARY_DIR}/inc/${TARGET_PLATFORM}/${CMAKE_BUILD_TYPE}/WslDeviceHost_p_${TARGET_PLATFORM}.c
    ${CMAKE_CURRENT_BINARY_DIR}/inc/${TARGET_PLATFORM}/${CMAKE_BUILD_TYPE}/dlldata_${TARGET_PLATFORM}.c
    ${CMAKE_CURRENT_LIST_DIR}/WslDeviceHostProxyStub.def
    ${CMAKE_CURRENT_LIST_DIR}/WslDeviceHostProxyStub.rc)

set_source_files_properties(${SOURCES} PROPERTIES GENERATED TRUE)

add_library(wsldevicehostproxystub SHARED ${SOURCES})
add_dependencies(wsldevicehostproxystub wsldevicehostidl)

# The generated dlldata.c uses ENTRY_PREFIX/REGISTER_PROXY_DLL only when those
# macros are pre-defined. midl-generated proxy code resolves IIDs via the _i.c
# definitions and links against rpcrt4/ole32 like classic-COM proxies.
target_link_libraries(wsldevicehostproxystub ${COMMON_LINK_LIBRARIES})

# The proxy/stub file uses `#define REGISTER_PROXY_DLL` so DllRegisterServer is
# emitted. This lets `regsvr32` work for developer/ad-hoc registration; the
# production install drives registration via MSI tables (see msipackage/package.wix.in).
target_compile_definitions(wsldevicehostproxystub PRIVATE
    REGISTER_PROXY_DLL
    PROXY_CLSID_IS={0x6D32A4B7,0x9E1F,0x4C82,{0xA5,0x73,0xF8,0xB1,0xC4,0xD2,0x9E,0x60}})

set_target_properties(wsldevicehostproxystub PROPERTIES FOLDER windows)

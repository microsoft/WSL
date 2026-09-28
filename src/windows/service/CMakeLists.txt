add_compile_definitions("PROXY_CLSID_IS={0x4EA0C6DD,0xE9FF,0x48E7,{0x99,0x4e,0x13,0xa3,0x1d,0x10,0xdc,0x60}}")
add_compile_definitions("REGISTER_PROXY_DLL")

add_subdirectory(exe)
add_subdirectory(inc)
add_subdirectory(stub)
add_subdirectory(mc)
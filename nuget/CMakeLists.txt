set(NUGET_PACKAGES Microsoft.WSL.PluginApi.nuspec)

# generate vars with native paths since nuget won't accept unix path separators
cmake_path(NATIVE_PATH CMAKE_SOURCE_DIR CMAKE_SOURCE_DIR_NATIVE)
cmake_path(NATIVE_PATH CMAKE_BINARY_DIR CMAKE_BINARY_DIR_NATIVE)

foreach(PACKAGE ${NUGET_PACKAGES})
    configure_file("${CMAKE_CURRENT_LIST_DIR}/${PACKAGE}.in" "${CMAKE_BINARY_DIR}/${PACKAGE}")
endforeach()
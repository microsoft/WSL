set(SOURCES
    ${CMAKE_CURRENT_BINARY_DIR}/../inc/${TARGET_PLATFORM}/${CMAKE_BUILD_TYPE}/wslservice_i_${TARGET_PLATFORM}.c
    ${CMAKE_CURRENT_BINARY_DIR}/../inc/${TARGET_PLATFORM}/${CMAKE_BUILD_TYPE}/wslservice_p_${TARGET_PLATFORM}.c
    ${CMAKE_CURRENT_BINARY_DIR}/../inc/${TARGET_PLATFORM}/${CMAKE_BUILD_TYPE}/dlldata_${TARGET_PLATFORM}.c
    ${CMAKE_CURRENT_LIST_DIR}/WslServiceProxyStub.def
    ${CMAKE_CURRENT_LIST_DIR}/WslServiceProxyStub.rc)

set_source_files_properties(${SOURCES} PROPERTIES GENERATED TRUE)

add_library(wslserviceproxystub SHARED ${SOURCES})
add_dependencies(wslserviceproxystub wslserviceidl)
target_link_libraries(wslserviceproxystub ${COMMON_LINK_LIBRARIES})
set_target_properties(wslserviceproxystub PROPERTIES FOLDER windows)
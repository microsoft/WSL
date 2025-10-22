set(SOURCES
    ${CMAKE_CURRENT_BINARY_DIR}/../inc/${TARGET_PLATFORM}/${CMAKE_BUILD_TYPE}/wslaservice_i_${TARGET_PLATFORM}.c
    ${CMAKE_CURRENT_BINARY_DIR}/../inc/${TARGET_PLATFORM}/${CMAKE_BUILD_TYPE}/wslaservice_p_${TARGET_PLATFORM}.c
    ${CMAKE_CURRENT_BINARY_DIR}/../inc/${TARGET_PLATFORM}/${CMAKE_BUILD_TYPE}/dlldata_${TARGET_PLATFORM}.c
    ${CMAKE_CURRENT_LIST_DIR}/WslaServiceProxyStub.def
    ${CMAKE_CURRENT_LIST_DIR}/WslaServiceProxyStub.rc)

set_source_files_properties(${SOURCES} PROPERTIES GENERATED TRUE)

add_library(wslaserviceproxystub SHARED ${SOURCES})
add_dependencies(wslaserviceproxystub wslaserviceidl)
target_link_libraries(wslaserviceproxystub ${COMMON_LINK_LIBRARIES})
set_target_properties(wslaserviceproxystub PROPERTIES FOLDER windows)
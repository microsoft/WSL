set(SOURCES
    application.manifest
    main.rc
    HcsVirtualMachine.cpp
    ServiceMain.cpp
    WSLASessionManager.cpp
    WSLASessionManagerFactory.cpp)

set(HEADERS
    WSLASessionManager.h
    WSLASessionManagerFactory.h
    HcsVirtualMachine.h)

add_executable(wslaservice ${SOURCES} ${HEADERS})
add_dependencies(wslaservice wslaserviceidl)
add_compile_definitions(__WRL_CLASSIC_COM__)
add_compile_definitions(__WRL_DISABLE_STATIC_INITIALIZE__)
add_compile_definitions(USE_COM_CONTEXT_DEF=1)
set_target_properties(wslaservice PROPERTIES LINK_FLAGS "/merge:minATL=.rdata /include:__minATLObjMap_WSLASessionManager_COM")
target_link_libraries(wslaservice
                      ${COMMON_LINK_LIBRARIES}
                      common
                      configfile
                      legacy_stdio_definitions)

target_precompile_headers(wslaservice REUSE_FROM common)
set_target_properties(wslaservice PROPERTIES FOLDER windows)

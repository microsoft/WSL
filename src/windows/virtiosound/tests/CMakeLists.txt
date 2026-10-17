function(wsl_virtio_sound_enable_test_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX /permissive- /EHsc)
    endif()
endfunction()

add_executable(wsl_virtio_sound_tests VirtioSoundDeviceTests.cpp)
target_link_libraries(wsl_virtio_sound_tests PRIVATE wsl_virtio_sound)
wsl_virtio_sound_enable_test_warnings(wsl_virtio_sound_tests)
add_test(NAME wsl_virtio_sound_core COMMAND wsl_virtio_sound_tests)

add_executable(wsl_virtio_sound_pcm_source_lifecycle_tests
    PcmSourceLifecycleTests.cpp)
target_link_libraries(wsl_virtio_sound_pcm_source_lifecycle_tests
    PRIVATE wsl_virtio_sound)
wsl_virtio_sound_enable_test_warnings(wsl_virtio_sound_pcm_source_lifecycle_tests)
add_test(NAME wsl_virtio_sound_pcm_source_lifecycle
    COMMAND wsl_virtio_sound_pcm_source_lifecycle_tests)

add_executable(wsl_virtio_sound_pcm_ring_tests S16LePcmRingTests.cpp)
target_link_libraries(wsl_virtio_sound_pcm_ring_tests PRIVATE wsl_virtio_sound)
wsl_virtio_sound_enable_test_warnings(wsl_virtio_sound_pcm_ring_tests)
add_test(NAME wsl_virtio_sound_pcm_ring COMMAND wsl_virtio_sound_pcm_ring_tests)

add_executable(wsl_virtio_sound_pcm_ring_core_integration_tests
    S16LePcmRingCoreIntegrationTests.cpp)
target_link_libraries(wsl_virtio_sound_pcm_ring_core_integration_tests
    PRIVATE wsl_virtio_sound)
wsl_virtio_sound_enable_test_warnings(wsl_virtio_sound_pcm_ring_core_integration_tests)
add_test(NAME wsl_virtio_sound_pcm_ring_core_integration
    COMMAND wsl_virtio_sound_pcm_ring_core_integration_tests)

if(WIN32)
    add_executable(wsl_virtio_sound_restricted_user_token_tests
        windows/RestrictedUserTokenTests.cpp)
    target_include_directories(wsl_virtio_sound_restricted_user_token_tests
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src/windows)
    target_compile_features(wsl_virtio_sound_restricted_user_token_tests
        PRIVATE cxx_std_17)
    target_compile_definitions(wsl_virtio_sound_restricted_user_token_tests
        PRIVATE _WIN32_WINNT=0x0601)
    target_link_libraries(wsl_virtio_sound_restricted_user_token_tests
        PRIVATE wsl_virtio_sound_windows_support advapi32)
    target_precompile_headers(wsl_virtio_sound_restricted_user_token_tests PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/../precomp.h)
    wsl_virtio_sound_enable_test_warnings(
        wsl_virtio_sound_restricted_user_token_tests)
    add_test(NAME wsl_virtio_sound_restricted_user_token
        COMMAND wsl_virtio_sound_restricted_user_token_tests)

    add_executable(wsl_virtio_sound_worker_pipe_security_tests
        windows/WorkerPipeSecurityTests.cpp)
    target_include_directories(wsl_virtio_sound_worker_pipe_security_tests
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src/windows)
    target_compile_features(wsl_virtio_sound_worker_pipe_security_tests
        PRIVATE cxx_std_17)
    target_compile_definitions(wsl_virtio_sound_worker_pipe_security_tests
        PRIVATE _WIN32_WINNT=0x0601)
    target_link_libraries(wsl_virtio_sound_worker_pipe_security_tests
        PRIVATE wsl_virtio_sound_windows_support advapi32)
    target_precompile_headers(wsl_virtio_sound_worker_pipe_security_tests PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/../precomp.h)
    wsl_virtio_sound_enable_test_warnings(
        wsl_virtio_sound_worker_pipe_security_tests)
    add_test(NAME wsl_virtio_sound_worker_pipe_security
        COMMAND wsl_virtio_sound_worker_pipe_security_tests)

    # Compile the adapter itself with its fake function table installed by
    # the test. This binary cannot fall back to loading the system HDV DLL.
    add_executable(wsl_virtio_sound_hdv_lifecycle_tests
        HdvPciDeviceHostTests.cpp
        ../src/VirtioSoundDevice.cpp
        ../src/windows/HdvPciDeviceHost.cpp)
    target_include_directories(wsl_virtio_sound_hdv_lifecycle_tests
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)
    target_compile_features(wsl_virtio_sound_hdv_lifecycle_tests
        PRIVATE cxx_std_17)
    target_compile_definitions(wsl_virtio_sound_hdv_lifecycle_tests
        PRIVATE WSL_VIRTIO_SOUND_ADAPTER_TESTING WIN32_LEAN_AND_MEAN NOMINMAX)
    target_link_libraries(wsl_virtio_sound_hdv_lifecycle_tests
        PRIVATE wsl_virtio_sound_windows_support advapi32 userenv wtsapi32 ole32)
    target_precompile_headers(wsl_virtio_sound_hdv_lifecycle_tests PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/../precomp.h)
    wsl_virtio_sound_enable_test_warnings(
        wsl_virtio_sound_hdv_lifecycle_tests)
    add_test(NAME wsl_virtio_sound_hdv_lifecycle
        COMMAND wsl_virtio_sound_hdv_lifecycle_tests)
endif()

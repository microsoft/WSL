cmake_minimum_required(VERSION 3.20)

project(wsl_virtio_sound_audio_worker LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "The audio capture worker backend requires Windows WASAPI.")
endif()

add_library(wsl_virtio_sound_audio_worker STATIC
    src/AudioCaptureWorker.cpp
    src/WasapiCaptureDevice.cpp)

target_include_directories(wsl_virtio_sound_audio_worker
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(wsl_virtio_sound_audio_worker PUBLIC cxx_std_17)
target_compile_definitions(wsl_virtio_sound_audio_worker
    PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0601)
target_precompile_headers(wsl_virtio_sound_audio_worker PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../precomp.h)
target_link_libraries(wsl_virtio_sound_audio_worker
    PRIVATE wsl_virtio_sound_windows_support advapi32 avrt ole32 uuid)

if(MSVC)
    target_compile_options(wsl_virtio_sound_audio_worker
        PRIVATE /W4 /WX /permissive- /EHsc)
endif()

option(WSL_AUDIO_WORKER_BUILD_TESTS "Build fake-device lifecycle tests" OFF)
if(WSL_AUDIO_WORKER_BUILD_TESTS)
    enable_testing()
    add_executable(wsl_virtio_sound_audio_worker_tests
        tests/AudioCaptureWorkerTests.cpp)
    target_compile_definitions(wsl_virtio_sound_audio_worker_tests
        PRIVATE _WIN32_WINNT=0x0601)
    target_precompile_headers(wsl_virtio_sound_audio_worker_tests PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/../precomp.h)
    target_link_libraries(wsl_virtio_sound_audio_worker_tests
        PRIVATE wsl_virtio_sound_audio_worker wsl_virtio_sound_windows_support)
    if(MSVC)
        target_compile_options(wsl_virtio_sound_audio_worker_tests
            PRIVATE /W4 /WX /permissive- /EHsc)
    endif()
    add_test(NAME wsl_virtio_sound_audio_worker_lifecycle
        COMMAND wsl_virtio_sound_audio_worker_tests)

    add_executable(wsl_virtio_sound_wasapi_initial_packet_policy_tests
        tests/WasapiInitialPacketPolicyTests.cpp)
    target_include_directories(wsl_virtio_sound_wasapi_initial_packet_policy_tests
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_compile_features(wsl_virtio_sound_wasapi_initial_packet_policy_tests
        PRIVATE cxx_std_17)
    target_compile_definitions(wsl_virtio_sound_wasapi_initial_packet_policy_tests
        PRIVATE _WIN32_WINNT=0x0601)
    target_link_libraries(wsl_virtio_sound_wasapi_initial_packet_policy_tests
        PRIVATE wsl_virtio_sound_windows_support)
    target_precompile_headers(wsl_virtio_sound_wasapi_initial_packet_policy_tests PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/../precomp.h)
    if(MSVC)
        target_compile_options(wsl_virtio_sound_wasapi_initial_packet_policy_tests
            PRIVATE /W4 /WX /permissive- /EHsc)
    endif()
    add_test(NAME wsl_virtio_sound_wasapi_initial_packet_policy
        COMMAND wsl_virtio_sound_wasapi_initial_packet_policy_tests)
endif()

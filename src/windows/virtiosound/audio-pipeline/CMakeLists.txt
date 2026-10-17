cmake_minimum_required(VERSION 3.20)
project(wsl_virtio_sound_audio_pipeline LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "The capture worker session uses the Windows WASAPI backend.")
endif()

add_subdirectory(../audio-ipc audio-ipc)
add_subdirectory(../audio-worker audio-worker)

add_library(wsl_virtio_sound_audio_worker_session STATIC
    ../audio-worker/src/AudioWorkerSession.cpp)
target_include_directories(wsl_virtio_sound_audio_worker_session PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../audio-worker/include)
target_compile_features(wsl_virtio_sound_audio_worker_session PUBLIC cxx_std_17)
target_compile_definitions(wsl_virtio_sound_audio_worker_session
    PRIVATE _WIN32_WINNT=0x0601)
target_precompile_headers(wsl_virtio_sound_audio_worker_session PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../precomp.h)
target_link_libraries(wsl_virtio_sound_audio_worker_session PUBLIC
    wsl_virtio_sound_audio_worker wsl_virtio_sound_audio_ipc
    PRIVATE wsl_virtio_sound_windows_support)
if(MSVC)
    target_compile_options(wsl_virtio_sound_audio_worker_session PRIVATE
        /W4 /WX /permissive- /EHsc)
endif()

add_executable(wsl-audio-worker ../audio-worker/src/AudioWorkerMain.cpp)
target_compile_features(wsl-audio-worker PRIVATE cxx_std_17)
target_compile_definitions(wsl-audio-worker
    PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE _WIN32_WINNT=0x0601)
target_precompile_headers(wsl-audio-worker PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../precomp.h)
target_link_libraries(wsl-audio-worker PRIVATE
    wsl_virtio_sound_audio_worker_session wsl_virtio_sound_audio_pipe
    wsl_virtio_sound_windows_support)
if(MSVC)
    target_compile_options(wsl-audio-worker PRIVATE /W4 /WX /permissive- /EHsc)
endif()

option(WSL_AUDIO_PIPELINE_BUILD_TESTS "Build worker session tests; never runs them at build time" OFF)
if(WSL_AUDIO_PIPELINE_BUILD_TESTS)
    enable_testing()
    add_executable(wsl_virtio_sound_audio_worker_session_tests
        ../audio-worker/tests/AudioWorkerSessionTests.cpp)
    target_compile_definitions(wsl_virtio_sound_audio_worker_session_tests
        PRIVATE _WIN32_WINNT=0x0601)
    target_precompile_headers(wsl_virtio_sound_audio_worker_session_tests PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/../precomp.h)
    target_link_libraries(wsl_virtio_sound_audio_worker_session_tests PRIVATE
        wsl_virtio_sound_audio_worker_session wsl_virtio_sound_windows_support)
    if(MSVC)
        target_compile_options(wsl_virtio_sound_audio_worker_session_tests PRIVATE
            /W4 /WX /permissive- /EHsc)
    endif()
    add_test(NAME wsl_virtio_sound_audio_worker_session
        COMMAND wsl_virtio_sound_audio_worker_session_tests)
endif()

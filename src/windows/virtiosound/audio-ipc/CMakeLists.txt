cmake_minimum_required(VERSION 3.20)

project(wsl_virtio_sound_audio_ipc LANGUAGES CXX)

add_library(wsl_virtio_sound_audio_ipc STATIC
    src/AudioIpcCodec.cpp)

target_include_directories(wsl_virtio_sound_audio_ipc
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(wsl_virtio_sound_audio_ipc PUBLIC cxx_std_17)

if(WIN32)
    target_precompile_headers(wsl_virtio_sound_audio_ipc PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/../precomp.h)
    target_link_libraries(wsl_virtio_sound_audio_ipc
        PRIVATE wsl_virtio_sound_windows_support)
endif()

if(MSVC)
    target_compile_options(wsl_virtio_sound_audio_ipc
        PRIVATE /W4 /WX /permissive- /EHsc)
endif()

if(WIN32)
    add_library(wsl_virtio_sound_audio_pipe STATIC
        src/windows/AudioPipeTransport.cpp)
    target_include_directories(wsl_virtio_sound_audio_pipe
        PUBLIC
            ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_compile_features(wsl_virtio_sound_audio_pipe PUBLIC cxx_std_17)
    target_compile_definitions(wsl_virtio_sound_audio_pipe
        PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0601)
    target_precompile_headers(wsl_virtio_sound_audio_pipe PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/../precomp.h)
    target_link_libraries(wsl_virtio_sound_audio_pipe
        PUBLIC wsl_virtio_sound_audio_ipc
        PRIVATE wsl_virtio_sound_windows_support)
    if(MSVC)
        target_compile_options(wsl_virtio_sound_audio_pipe
            PRIVATE /W4 /WX /permissive- /EHsc)
    endif()
endif()

option(WSL_AUDIO_IPC_BUILD_TESTS "Build audio IPC codec tests" OFF)
if(WSL_AUDIO_IPC_BUILD_TESTS)
    enable_testing()
    add_executable(wsl_virtio_sound_audio_ipc_tests
        tests/AudioIpcCodecTests.cpp)
    target_link_libraries(wsl_virtio_sound_audio_ipc_tests
        PRIVATE wsl_virtio_sound_audio_ipc)
    if(WIN32)
        target_link_libraries(wsl_virtio_sound_audio_ipc_tests
            PRIVATE wsl_virtio_sound_windows_support)
        target_precompile_headers(wsl_virtio_sound_audio_ipc_tests PRIVATE
            ${CMAKE_CURRENT_LIST_DIR}/../precomp.h)
    endif()
    if(MSVC)
        target_compile_options(wsl_virtio_sound_audio_ipc_tests
            PRIVATE /W4 /WX /permissive- /EHsc)
    endif()
    add_test(NAME wsl_virtio_sound_audio_ipc_codec
        COMMAND wsl_virtio_sound_audio_ipc_tests)
endif()

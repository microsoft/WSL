cmake_minimum_required(VERSION 3.20)
project(wsl_virtio_sound LANGUAGES CXX)

if(WIN32)
    # The enclosing WSL project also provides a precomp.h. Resolve this
    # subtree's explicitly included PCH before inherited include directories.
    include_directories(BEFORE "${CMAKE_CURRENT_SOURCE_DIR}")
    # The repository build already resolves WIL from its NuGet package. Keep
    # the standalone subtree buildable as well, while allowing an explicit
    # include path for offline builds.
    set(WSL_VIRTIO_SOUND_WIL_INCLUDE_DIR "" CACHE PATH "Path to the WIL include directory")
    if(NOT WSL_VIRTIO_SOUND_WIL_INCLUDE_DIR)
        if(DEFINED WIL_SOURCE_DIR AND EXISTS "${WIL_SOURCE_DIR}/include/wil/resource.h")
            set(WSL_VIRTIO_SOUND_WIL_INCLUDE_DIR "${WIL_SOURCE_DIR}/include")
        else()
            include(FetchContent)
            FetchContent_Declare(wsl_virtio_sound_wil
                URL https://github.com/microsoft/wil/archive/b6ec76a2d9a609897f25a7fa0a0bdf4238e94e35.tar.gz
                URL_HASH SHA256=1573c54f254284b8ba7d3f1aa73d328122075ce4adc28276df440b7a37e9e315
                # Consume headers without configuring WIL's own test project.
                SOURCE_SUBDIR include)
            FetchContent_MakeAvailable(wsl_virtio_sound_wil)
            set(WSL_VIRTIO_SOUND_WIL_INCLUDE_DIR "${wsl_virtio_sound_wil_SOURCE_DIR}/include")
        endif()
    endif()

    if(NOT EXISTS "${WSL_VIRTIO_SOUND_WIL_INCLUDE_DIR}/wil/resource.h")
        message(FATAL_ERROR
            "WIL headers were not found. Set WSL_VIRTIO_SOUND_WIL_INCLUDE_DIR to the directory containing wil/resource.h.")
    endif()

    add_library(wsl_virtio_sound_windows_support INTERFACE)
    target_include_directories(wsl_virtio_sound_windows_support INTERFACE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${WSL_VIRTIO_SOUND_WIL_INCLUDE_DIR}")
endif()

add_library(wsl_virtio_sound STATIC
    src/VirtioSoundDevice.cpp
    src/S16LePcmRing.cpp)

target_include_directories(wsl_virtio_sound
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(wsl_virtio_sound PUBLIC cxx_std_17)

option(WSL_VIRTIO_SOUND_BUILD_TESTS "Build the offline virtio-sound test suite" OFF)

if(WSL_VIRTIO_SOUND_BUILD_TESTS)
    enable_testing()
    # One opt-in builds every offline suite, including tests owned by the
    # portable IPC and Windows worker subprojects.
    set(WSL_AUDIO_IPC_BUILD_TESTS ON)
    set(WSL_AUDIO_WORKER_BUILD_TESTS ON)
    set(WSL_AUDIO_PIPELINE_BUILD_TESTS ON)
endif()

if(WIN32)
    add_subdirectory(audio-pipeline audio-pipeline)
    target_sources(wsl_virtio_sound PRIVATE
        src/windows/HdvPciDeviceHost.cpp
        src/windows/WindowsCaptureSource.cpp)
    target_compile_definitions(wsl_virtio_sound PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
    target_precompile_headers(wsl_virtio_sound PRIVATE precomp.h)
    set_source_files_properties(src/VirtioSoundDevice.cpp src/S16LePcmRing.cpp
        PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    target_link_libraries(wsl_virtio_sound
        PUBLIC wsl_virtio_sound_audio_pipe
        PRIVATE wsl_virtio_sound_windows_support advapi32 userenv wtsapi32 ole32)
elseif(WSL_VIRTIO_SOUND_BUILD_TESTS)
    # The codec is portable and remains testable on non-Windows hosts even
    # though the production pipe and capture worker require Windows.
    add_subdirectory(audio-ipc audio-ipc)
endif()

if(WSL_VIRTIO_SOUND_BUILD_TESTS)
    add_subdirectory(tests)
endif()

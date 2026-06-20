set(FUZZING_OUT_DIR ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${CMAKE_BUILD_TYPE}/fuzzing)

function(add_fuzzing_harness TARGET)
    cmake_parse_arguments(ARG "" "TARGET_NAME" "LINK_LIBRARIES;EXTRA_DEPENDENCIES;INCLUDE_DIRECTORIES" ${ARGN})

    add_executable(${TARGET} ${TARGET}.cpp)
    target_precompile_headers(${TARGET} REUSE_FROM common)

    if (NOT WSL_ENABLE_FUZZING_REPLAY)
        target_compile_options(${TARGET} PRIVATE /fsanitize=fuzzer)
    endif()

    if(ARG_LINK_LIBRARIES)
        target_link_libraries(${TARGET} PRIVATE ${ARG_LINK_LIBRARIES})
    endif()

    if(ARG_INCLUDE_DIRECTORIES)
        target_include_directories(${TARGET} PRIVATE ${ARG_INCLUDE_DIRECTORIES})
    endif()

    set_target_properties(${TARGET} PROPERTIES FOLDER fuzzing)

    if(ARG_TARGET_NAME)
        set(TARGET_NAME ${ARG_TARGET_NAME})
    else()
        set(TARGET_NAME ${TARGET})
    endif()

    # Add extra dependencies to the entry, and stage them into the shared fuzzing output dir
    set(EXTRA_DEPENDENCIES_JSON "")
    foreach(dep IN LISTS ARG_EXTRA_DEPENDENCIES)
        string(APPEND EXTRA_DEPENDENCIES_JSON ", \"${dep}\"")

        # Dependency file will be in the main binary output dir after build
        add_custom_command(TARGET ${TARGET} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${CMAKE_BUILD_TYPE}/${dep}
                ${FUZZING_OUT_DIR}/${dep})
    endforeach()

    file(READ ${CMAKE_CURRENT_SOURCE_DIR}/OneFuzzConfigEntry.json.in ENTRY_TEMPLATE)
    string(CONFIGURE "${ENTRY_TEMPLATE}" ENTRY_JSON)

    # Append this harness's entry to OneFuzzConfig.json (written after all harnesses).
    list(APPEND ONEFUZZ_ENTRIES "${ENTRY_JSON}")
    set(ONEFUZZ_ENTRIES "${ONEFUZZ_ENTRIES}" PARENT_SCOPE)
endfunction()

configure_file(setup.ps1 ${FUZZING_OUT_DIR}/setup.ps1 COPYONLY)

add_fuzzing_harness(WslcCliArgumentFuzzing
    TARGET_NAME cliArgumentParser
    LINK_LIBRARIES wslclib)

add_fuzzing_harness(WslcSdkFuzzing
    TARGET_NAME sdkApiFlow
    LINK_LIBRARIES wslcsdk
    INCLUDE_DIRECTORIES ${CMAKE_SOURCE_DIR}/src/windows/WslcSDK
    EXTRA_DEPENDENCIES "wslcsdk.dll" "wslcsdk.pdb")

add_fuzzing_harness(WslcWinRtFuzzing
    TARGET_NAME winrtApiFlow
    LINK_LIBRARIES wslcsdk
    INCLUDE_DIRECTORIES ${CMAKE_SOURCE_DIR}/src/windows/WslcSDK ${CMAKE_BINARY_DIR}/src/windows/WslcSDK/winrt/${TARGET_PLATFORM}/${CMAKE_BUILD_TYPE}
    EXTRA_DEPENDENCIES "wslcsdk.dll" "wslcsdk.pdb")

# Write OneFuzzConfig.json with one entry per harness.
list(JOIN ONEFUZZ_ENTRIES ",\n" ONEFUZZ_ENTRIES)
configure_file(OneFuzzConfig.json.in ${FUZZING_OUT_DIR}/OneFuzzConfig.json)

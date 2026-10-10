set(OUT ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/cloudtest)
file(MAKE_DIRECTORY ${OUT})

if (${TARGET_PLATFORM} STREQUAL x64)
    set(CLOUDTEST_IMAGES
        "wsl-test-image-rs_prerelease-2026-10-05"
        "wsl-test-image-win11-23h2-ent-2024-11-18"
        "wsl-test-image-2022-datacenter-g2-2024-09-10"
        "wsl-test-image-win10-22h2-ent-g2-2024-09-10")

    # WSLC tests trigger a Hyper-V VMBus PowerOff hang during VM teardown
    # on Windows Server SKUs, which causes the WSLC TAEF job to time out.
    # Skip the WSLC test suite on Server images until the host issue is
    # resolved.
    set(CLOUDTEST_SERVER_IMAGES
        "wsl-test-image-2022-datacenter-g2-2024-09-10")

    set(CLOUDTEST_TEST_PACKAGES "Test_Packages_2025_07_28")
    set(DUMPTOOL_DROP "DumpTool_X64_2025-01-27")
elseif (${TARGET_PLATFORM} STREQUAL arm64)
    set(CLOUDTEST_IMAGES)
    set(CLOUDTEST_SERVER_IMAGES)
else()
    message(FATAL_ERROR "Unsupported target platform: ${TARGET_PLATFORM}")
endif()

# Passed down to test-setup.ps1 to determine if -AllowUnsigned should be passed to Add-AppxPackage
if (OFFICIAL_BUILD)
    set(ALLOW_UNSIGNED_PACKAGE "0")
else()
    set(ALLOW_UNSIGNED_PACKAGE "1")
endif()

# When the package stage is included (release + nightly), test the real bundle from that stage.
# PR builds test the installer.msix directly from the build artifact (no bundle needed).
if (INCLUDE_PACKAGE_STAGE)
    set(TEST_PACKAGE_PROVIDER "[package]")
    set(TEST_PACKAGE_PATH "bundle\\Microsoft.WSL_${PACKAGE_VERSION}_x64_ARM64.msixbundle")
    set(TEST_PACKAGE_FILE "Microsoft.WSL_${PACKAGE_VERSION}_x64_ARM64.msixbundle")
else()
    set(TEST_PACKAGE_PROVIDER "[drop]")
    set(TEST_PACKAGE_PATH "testbin\\x64\\release\\installer.msix")
    set(TEST_PACKAGE_FILE "installer.msix")
endif()

function(add_test_group image version suffix filter backend_argument)
    set(DIR ${OUT}/${image}-${suffix})
    file(MAKE_DIRECTORY ${DIR})

    set(BACKEND_ARGUMENT ${backend_argument})
    configure_file(${CMAKE_CURRENT_SOURCE_DIR}/TestMap.xml.in ${DIR}/TestMap.xml)
    configure_file(${CMAKE_CURRENT_SOURCE_DIR}/TestGroup.xml.in ${DIR}/TestGroup.xml)
endfunction()

foreach(image ${CLOUDTEST_IMAGES})
    # OpenVMM backend tests construct OpenVMM VMs directly, so they belong only in the dedicated
    # OpenVMM pass below, which runs on an image that has the Windows Hypervisor Platform enabled.
    set(TEST_FILTER "not (@TestCategory='WSLC' or @TestCategory='OpenVMM')")

    add_test_group("${image}" "1" "wsl1" "${TEST_FILTER}" "")
    add_test_group("${image}" "2" "wsl2" "${TEST_FILTER}" "")
    if (NOT "${image}" IN_LIST CLOUDTEST_SERVER_IMAGES)
        add_test_group("${image}" "2" "wslc" "@TestCategory='WSLC'" "")
    endif()

    if ("${image}" STREQUAL "wsl-test-image-rs_prerelease-2026-10-05")
        # Re-run the WSL2 suite against the OpenVMM backend, minus the tests whose outcome cannot
        # depend on the backend in use, which would otherwise duplicate the wsl2 pass above.
        add_test_group("${image}" "2" "wsl2-openvmm" "not (@TestCategory='WSLC' or @TestCategory='BackendAgnostic')" "/p:OpenVmm=1")
    endif()
endforeach()

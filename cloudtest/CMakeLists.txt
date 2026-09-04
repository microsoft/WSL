set(OUT ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/cloudtest)
file(MAKE_DIRECTORY ${OUT})

if (${TARGET_PLATFORM} STREQUAL x64)
    set(CLOUDTEST_IMAGES
        "wsl-test-image-rs_prerelease-2025-01-30"
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
    set(TEST_PACKAGE_FILE "Microsoft.WSL_${PACKAGE_VERSION}_x64_ARM64.msixbundle")
    set(TEST_PACKAGE_BUILD_FILE "      <Copy Src=\"[package]\\bundle\\${TEST_PACKAGE_FILE}\" Dest=\"[WorkingDirectory]\" IsRecursive=\"false\"/>")
    set(PACKAGE_ARTIFACT_PROVIDER "    <Provider Type=\"PipelineArtifacts\">
      <Properties>
        <Add Name=\"CloudTest.ProviderCustomName\" Value=\"[package]\" />
        <Add Name=\"PipelineArtifactBuildUrl\" Value=\"https://${VSO_ORG}.visualstudio.com/${VSO_PROJECT}/_build/results?buildId=${PIPELINE_BUILD_ID}\"/>
        <Add Name=\"PipelineArtifactName\" Value=\"drop_wsl_package\"/>
      </Properties>
    </Provider>")
else()
    set(TEST_PACKAGE_FILE "installer.msix")
    set(TEST_PACKAGE_BUILD_FILE "")
    set(PACKAGE_ARTIFACT_PROVIDER "")
endif()

function(add_test_group image version suffix filter)
    cmake_parse_arguments(
        PARSE_ARGV 4
        TEST_GROUP
        "INCLUDE_UNIT_TESTS;INCLUDE_TEST_DATA;INCLUDE_TEST_PACKAGES"
        ""
        "")

    set(UNIT_TESTS_BUILD_FILE "")
    set(UNIT_TESTS_ARGUMENT "")
    if (TEST_GROUP_INCLUDE_UNIT_TESTS)
        set(UNIT_TESTS_BUILD_FILE "      <Copy Src=\"[drop]\\testbin\\unit_tests\\*\" Dest=\"[WorkingDirectory]\\unit_tests\" IsRecursive=\"true\" Writable=\"true\"/>")
        set(UNIT_TESTS_ARGUMENT " /p:UnitTestsPath=[WorkingDirectory]\\unit_tests")
    endif()

    set(TEST_DATA_BUILD_FILE "")
    if (TEST_GROUP_INCLUDE_TEST_DATA)
        set(TEST_DATA_BUILD_FILE "      <Copy Src=\"[drop]\\testbin\\${TARGET_PLATFORM}\\test_data\\*\" Dest=\"[WorkingDirectory]\\test_data\" IsRecursive=\"true\" Writable=\"true\"/>")
    endif()

    set(TEST_PACKAGES_BUILD_FILE "")
    set(TEST_PACKAGES_PROVIDER "")
    if (TEST_GROUP_INCLUDE_TEST_PACKAGES)
        set(TEST_PACKAGES_BUILD_FILE "      <Copy Src=\"[test_packages]\\*\" Dest=\"[WorkingDirectory]\" IsRecursive=\"false\"/>")
        set(TEST_PACKAGES_PROVIDER "    <Provider Type=\"VSODrop\">
      <Properties>
        <Add Name=\"CloudTest.ProviderCustomName\" Value=\"[test_packages]\" />
        <!-- When updating the drops for this make sure the retention is set to never expire -->
        <Add Name=\"DropURL\" Value=\"https://${VSO_ORG}.artifacts.visualstudio.com/_apis/drop/drops/WSL/${CLOUDTEST_TEST_PACKAGES}\"/>
      </Properties>
    </Provider>")
    endif()

    set(DIR ${OUT}/${image}-${suffix})
    file(MAKE_DIRECTORY ${DIR})

    configure_file(${CMAKE_CURRENT_SOURCE_DIR}/TestMap.xml.in ${DIR}/TestMap.xml)
    configure_file(${CMAKE_CURRENT_SOURCE_DIR}/TestGroup.xml.in ${DIR}/TestGroup.xml)
endfunction()

set(NON_WSLC_FILTER "not (@TestCategory='WSLC')")

string(CONCAT WSL2_UNIT_CLASSES
    "("
    "@Name='UnitTests::UnitTests::*' OR "
    "@Name='FilesystemUnitTests::FilesystemUnitTests::*' OR "
    "@Name='StringUnitTests::StringUnitTests::*' OR "
    "@Name='WindowsUpdateTests::*' OR "
    "@Name='NetworkTests::GnsCallbackResultTests::*'"
    ")")

string(CONCAT WSL2_E2E_1_CLASSES
    "("
    "@Name='NetworkTests::MirroredTests::*' OR "
    "@Name='MountTests::MountTests::*' OR "
    "@Name='NetworkTests::ConsommeTests::*' OR "
    "@Name='Plan9Tests::Plan9Tests::*' OR "
    "@Name='PolicyTest::*' OR "
    "@Name='SimpleTests::SimpleTests::*'"
    ")")

set(WSL2_UNIT_FILTER "${NON_WSLC_FILTER} AND ${WSL2_UNIT_CLASSES}")
set(WSL2_E2E_1_FILTER "${NON_WSLC_FILTER} AND ${WSL2_E2E_1_CLASSES}")
set(WSL2_E2E_2_FILTER "${NON_WSLC_FILTER} AND NOT ${WSL2_UNIT_CLASSES} AND NOT ${WSL2_E2E_1_CLASSES}")

foreach(image ${CLOUDTEST_IMAGES})
    add_test_group("${image}" "1" "wsl1" "${NON_WSLC_FILTER}" INCLUDE_UNIT_TESTS INCLUDE_TEST_PACKAGES)
    add_test_group("${image}" "2" "wsl2-unit" "${WSL2_UNIT_FILTER}" INCLUDE_UNIT_TESTS)
    add_test_group("${image}" "2" "wsl2-e2e-1" "${WSL2_E2E_1_FILTER}" INCLUDE_UNIT_TESTS)
    add_test_group(
        "${image}"
        "2"
        "wsl2-e2e-2"
        "${WSL2_E2E_2_FILTER}"
        INCLUDE_UNIT_TESTS
        INCLUDE_TEST_DATA
        INCLUDE_TEST_PACKAGES)
    if (NOT "${image}" IN_LIST CLOUDTEST_SERVER_IMAGES)
        add_test_group("${image}" "2" "wslc" "@TestCategory='WSLC'" INCLUDE_TEST_DATA)
    endif()
endforeach()

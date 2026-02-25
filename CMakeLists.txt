cmake_minimum_required(VERSION 3.25)
set(CMAKE_SYSTEM_VERSION 10.0.26100.0)

project(wsl)

# Rationalize TARGET_PLATFORM
if("${CMAKE_GENERATOR_PLATFORM}" STREQUAL "arm64" OR "${TARGET_PLATFORM}" STREQUAL "arm64")
    set(TARGET_PLATFORM "arm64")
    set(TEST_DISTRO_PLATFORM "arm64")
elseif("${CMAKE_GENERATOR_PLATFORM}" MATCHES "x64|amd64|" OR "${TARGET_PLATFORM}" MATCHES "x64|amd64|")
    set(TARGET_PLATFORM "x64")
    set(TEST_DISTRO_PLATFORM "amd64")
else()
    message(FATAL_ERROR "Unsupported platform: ${CMAKE_GENERATOR_PLATFORM}")
endif()


if (NOT ${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION} STREQUAL ${CMAKE_SYSTEM_VERSION})
    message(FATAL_ERROR "Incorrect Windows SDK version: ${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}, requires ${CMAKE_SYSTEM_VERSION}")
endif()

include(FetchContent)

# Import GSL and nlohmannjson

set(FETCHCONTENT_BASE_DIR ${CMAKE_BINARY_DIR}/_deps/${TARGET_PLATFORM})

FetchContent_Declare(GSL
                     URL https://github.com/microsoft/GSL/archive/refs/tags/v4.0.0.tar.gz
                     URL_HASH SHA256=f0e32cb10654fea91ad56bde89170d78cfbf4363ee0b01d8f097de2ba49f6ce9)

FetchContent_MakeAvailable(GSL)
FetchContent_GetProperties(GSL SOURCE_DIR GSL_SOURCE_DIR)


FetchContent_Declare(nlohmannjson
                     URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
                     URL_HASH SHA256=42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa )

FetchContent_MakeAvailable(nlohmannjson)
FetchContent_GetProperties(nlohmannjson SOURCE_DIR NLOHMAN_JSON_SOURCE_DIR)

# Import modules
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake")
find_package(IDL REQUIRED)
find_package(LINUXBUILD REQUIRED)
find_package(NUGET REQUIRED)
find_package(VERSION REQUIRED)
find_package(MC REQUIRED)
find_package(Appx REQUIRED)

# Download nuget packages
restore_nuget_packages()

# Load nuget packages versions and paths
parse_nuget_packages_versions()

find_nuget_package(Microsoft.Direct3D.Linux DIRECT3D /build/native)
find_nuget_package(Microsoft.Identity.MSAL.WSL.Proxy MSAL /build/native/bin)
find_nuget_package(Microsoft.RemoteDesktop.Client.MSRDC.SessionHost MSRDC /build/native/bin)
find_nuget_package(Microsoft.Taef TAEF /)
find_nuget_package(Microsoft.Windows.ImplementationLibrary WIL /)
find_nuget_package(Microsoft.WSL.DeviceHost WSL_DEVICE_HOST /build/native)
find_nuget_package(Microsoft.WSL.Kernel KERNEL /build/native)
find_nuget_package(Microsoft.WSL.bsdtar BSDTARD /build/native/bin)
find_nuget_package(Microsoft.WSL.LinuxSdk LINUXSDK /)
find_nuget_package(Microsoft.WSL.TestDistro TEST_DISTRO /)
find_nuget_package(Microsoft.WSLg WSLG /build/native/bin)
find_nuget_package(vswhere VSWHERE /tools)
find_nuget_package(Wix WIX /tools/net6.0/any)

# Architecture-specific nuget packages from the OS repo.
if (${TARGET_PLATFORM} STREQUAL "x64")
    find_nuget_package(Microsoft.DXCore.Linux.amd64fre DXCORE /build/native)
    find_nuget_package(Microsoft.WSL.Dependencies.amd64fre WSLDEPS /build/native)
endif()

if (${TARGET_PLATFORM} STREQUAL "arm64")
    find_nuget_package(Microsoft.DXCore.Linux.arm64fre DXCORE /build/native)
    find_nuget_package(Microsoft.WSL.Dependencies.arm64fre WSLDEPS /build/native)
endif()

# Wsl Settings packages
find_nuget_package(CommunityToolkit.Mvvm CTK_MVVM /)
find_nuget_package(CommunityToolkit.WinUI.Animations CTK_ANIMATIONS /)
find_nuget_package(CommunityToolkit.WinUI.Controls.SettingsControls CTK_STTNGS_CTRLS /)
find_nuget_package(Microsoft.Extensions.Hosting EXTS_HOSTING /)
find_nuget_package(Microsoft.NETCore.App.Runtime.win-${TARGET_PLATFORM} DOTNET_RUNTIME /)
find_nuget_package(Microsoft.WindowsAppSDK WIN_APP_SDK /)
find_nuget_package(Microsoft.Windows.SDK.NET.Ref WINDOWS_SDK_DOTNET /)
find_nuget_package(Microsoft.Xaml.Behaviors.WinUI.Managed XAML_BEHAVIORS /)
find_nuget_package(WinUIEx WINUIEX /)

set(WSLG_TS_PLUGIN_DLL "WSLDVCPlugin.dll")

# Default to debug build if unspecified
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE "Debug")
endif()

set(SUPPORTED_LANGS cs-CZ;da-DK;de-DE;en-GB;en-US;es-ES;fi-FI;fr-FR;hu-HU;it-IT;ja-JP;ko-KR;nb-NO;nl-NL;pl-PL;pt-BR;pt-PT;ru-RU;sv-SE;tr-TR;zh-CN;zh-TW)

if (EXISTS "${CMAKE_CURRENT_LIST_DIR}/UserConfig.cmake")
    find_package(USER REQUIRED PATHS ${CMAKE_CURRENT_LIST_DIR})
endif()

# Optional target configuration

if (NOT DEFINED WSL_BUILD_WSL_SETTINGS)
    set(WSL_BUILD_WSL_SETTINGS false)
endif ()

# Only generate the build configuration that CMake is configured for
set(CMAKE_CONFIGURATION_TYPES "${CMAKE_BUILD_TYPE}" CACHE STRING "" FORCE)

find_commit_hash(COMMIT_HASH)

if (NOT PACKAGE_VERSION)
    find_version(PACKAGE_VERSION WSL_NUGET_PACKAGE_VERSION) # Fetch the package version from git if not specified
endif ()

if (NOT PACKAGE_VERSION MATCHES "^([0-9]+).([0-9]+).([0-9]+).([0-9]+)$")
    message(FATAL_ERROR "PACKAGE_VERSION is invalid: '${PACKAGE_VERSION}'. Needs to match '([0-9]+).([0-9]+).([0-9]+).([0-9]+)'")
endif()

set(PACKAGE_VERSION_MAJOR ${CMAKE_MATCH_1})
set(PACKAGE_VERSION_MINOR ${CMAKE_MATCH_2})
set(PACKAGE_VERSION_REVISION ${CMAKE_MATCH_3})

# The store requires the revision number to be 0, so enforce this on official builds
if (OFFICIAL_BUILD AND NOT PACKAGE_VERSION MATCHES "^([0-9]+).([0-9]+).([0-9]+).0$")
    message(FATAL_ERROR "PACKAGE_VERSION is invalid: '${PACKAGE_VERSION}'. Needs to match '([0-9]+).([0-9]+).([0-9]+).0' for official builds")
endif()

# Configure output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin/${TARGET_PLATFORM})
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/Debug)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/Release)

set_property(GLOBAL PROPERTY USE_FOLDERS ON)

# Packaging variables
set(BIN ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${CMAKE_BUILD_TYPE})
file(MAKE_DIRECTORY ${BIN})

set (GENERATED_DIR ${CMAKE_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${GENERATED_DIR})

set(PACKAGE_CERTIFICATE ${GENERATED_DIR}/dev-cert.pfx)
file(CREATE_LINK ${WSL_DEVICE_HOST_SOURCE_DIR}/bin/${TARGET_PLATFORM}/wsldevicehost.dll ${BIN}/wsldevicehost.dll)
file(CREATE_LINK ${WSLG_SOURCE_DIR}/${TARGET_PLATFORM}/${WSLG_TS_PLUGIN_DLL} ${BIN}/${WSLG_TS_PLUGIN_DLL})
file(CREATE_LINK ${WSLDEPS_SOURCE_DIR}/bin/wsldeps.dll ${BIN}/wsldeps.dll)

if (${SKIP_PACKAGE_SIGNING})
    set(PACKAGE_SIGN_COMMAND echo Skipped package signing for:)
else()
    if (NOT EXISTS ${PACKAGE_CERTIFICATE})
        execute_process(
            COMMAND powershell.exe -ExecutionPolicy Bypass -NoProfile -NonInteractive ${CMAKE_CURRENT_LIST_DIR}/tools/create-dev-cert.ps1 -OutputPath ${PACKAGE_CERTIFICATE}
            COMMAND_ERROR_IS_FATAL ANY)
    endif()

    set(PACKAGE_SIGN_COMMAND SignTool.exe sign /a /v /fd SHA256 /f ${PACKAGE_CERTIFICATE})
endif()


# Generate local test script
configure_file(${CMAKE_CURRENT_LIST_DIR}/tools/test/test.bat.in  ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${CMAKE_BUILD_TYPE}/test.bat)

# Common build flags
set(CMAKE_CXX_STANDARD 20)

if (${CMAKE_BUILD_TYPE} STREQUAL "Debug")
    set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreadedDebug)
else()
    set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreaded)
endif()

if (${TARGET_PLATFORM} STREQUAL "x64")
    add_compile_definitions(_AMD64_)
endif()

if (${TARGET_PLATFORM} STREQUAL "arm64")
    add_compile_definitions(_ARM64_)
endif()

add_definitions(/sdl) # Default-initialize class members
add_definitions(/FS) # Enable parallel PDB access
add_compile_definitions(UNICODE
                        WIL_SUPPRESS_PRIVATE_API_USE
                        CPPWINRT_SUPPRESS_STATIC_INITIALIZERS
                        NOMINMAX
                        _CRT_SECURE_NO_WARNINGS
                        KERNEL_VERSION="${KERNEL_VERSION}"
                        WSLDEPS_VERSION="${WSLDEPS_VERSION}"
                        WSLG_VERSION="${WSLG_VERSION}"
                        WSLG_TS_PLUGIN_DLL=L"${WSLG_TS_PLUGIN_DLL}"
                        WSL_DEVICE_HOST_VERSION="${WSL_DEVICE_HOST_VERSION}"
                        COMMIT_HASH="${COMMIT_HASH}"
                        WSL_PACKAGE_VERSION="${PACKAGE_VERSION}"
                        MSRDC_VERSION="${MSRDC_VERSION}"
                        DIRECT3D_VERSION="${DIRECT3D_VERSION}"
                        DXCORE_VERSION="${DXCORE_VERSION}"
                        WSL_PACKAGE_VERSION_MAJOR=${PACKAGE_VERSION_MAJOR}
                        WSL_PACKAGE_VERSION_MINOR=${PACKAGE_VERSION_MINOR}
                        WSL_PACKAGE_VERSION_REVISION=${PACKAGE_VERSION_REVISION}
                        WSL_BUILD_WSL_SETTINGS=${WSL_BUILD_WSL_SETTINGS})

if (${OFFICIAL_BUILD})
    add_compile_definitions(WSL_OFFICIAL_BUILD)
endif()

if (${WSL_BUILD_THIN_PACKAGE})
    add_compile_definitions(WSL_DEV_THIN_MSI_PACKAGE="${BIN}/wsl.msi")
endif ()

string(REPLACE "/Zi" "" CMAKE_CXX_FLAGS_DEBUG ${CMAKE_CXX_FLAGS_DEBUG})  # make sure /Zi is removed from the debug flags
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /bigobj /W3 /WX /ZH:SHA_256")
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} /Z7 -DDEBUG -DDBG")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /Zi /guard:cf /Qspectre")

# Linker flags
set(CMAKE_SHARED_LINKER_FLAGS_RELEASE "${CMAKE_SHARED_LINKER_FLAGS_RELEASE} /debug:full /debugtype:cv,fixup /guard:cf /DYNAMICBASE")
set(CMAKE_EXE_LINKER_FLAGS_RELEASE "${CMAKE_EXE_LINKER_FLAGS_RELEASE} /debug:full /debugtype:cv,fixup /guard:cf /DYNAMICBASE")
if (${TARGET_PLATFORM} STREQUAL "x64")
    set(CMAKE_SHARED_LINKER_FLAGS_RELEASE "${CMAKE_SHARED_LINKER_FLAGS_RELEASE} /CETCOMPAT")
    set(CMAKE_EXE_LINKER_FLAGS_RELEASE "${CMAKE_EXE_LINKER_FLAGS_RELEASE} /CETCOMPAT")
endif()

# Common link libraries
link_directories(${WSLDEPS_SOURCE_DIR}/lib/)
set(COMMON_LINK_LIBRARIES
    ws2_32.lib
    Userenv.lib
    RuntimeObject.lib
    Pathcch.lib
    ntdll.lib
    RpcRT4.lib
    Mswsock.lib
    Shlwapi.lib
    synchronization.lib
    Bcrypt.lib
    Iphlpapi.lib
    icu.lib
    computecore.lib
    computenetwork.lib
    MI.lib
    Wintrust.lib
    msi.lib
    wsldeps.lib)

# Linux
if(${TARGET_PLATFORM} STREQUAL "" OR ${TARGET_PLATFORM} STREQUAL "x64")
    set(LLVM_ARCH x86_64)
elseif(${TARGET_PLATFORM} STREQUAL "arm64")
    set(LLVM_ARCH aarch64)
else()
    message(FATAL_ERROR "Unsupported platform: '${TARGET_PLATFORM}'")
endif()

# Determine the Visual Studio installation directory which contains LLVM tools
# N.B. The version is set to VS2022 to ensure local runs match pipeline behavior
# Require that Clang be installed in the product to potentially de-deduplicate
execute_process(
    COMMAND "${VSWHERE_SOURCE_DIR}/vswhere.exe" -version "[17.0,18.0)" -products * -requires Microsoft.VisualStudio.Component.VC.Llvm.Clang -property installationPath
    OUTPUT_VARIABLE VS_INSTALL_DIR
    OUTPUT_STRIP_TRAILING_WHITESPACE
    COMMAND_ERROR_IS_FATAL ANY
)

if (NOT VS_INSTALL_DIR)
    message(FATAL_ERROR "Could not determine Visual Studio 2022 installation directory.")
endif()

if("${CMAKE_HOST_SYSTEM_PROCESSOR}" STREQUAL "AMD64")
    set(LLVM_INSTALL_DIR "${VS_INSTALL_DIR}/VC/Tools/Llvm/x64/bin")
else()
    set(LLVM_INSTALL_DIR "${VS_INSTALL_DIR}/VC/Tools/Llvm/${CMAKE_HOST_SYSTEM_PROCESSOR}/bin")
endif()

if (NOT EXISTS ${LLVM_INSTALL_DIR})
    message(FATAL_ERROR "C++ Clang Compiler for Windows is not installed at ${LLVM_INSTALL_DIR}. Please install it from the Visual Studio Installer.")
endif()

# Generate the clang-format script which contains a path to clang-format.exe
configure_file(${CMAKE_CURRENT_LIST_DIR}/tools/FormatSource.ps1.in ${CMAKE_BINARY_DIR}/FormatSource.ps1)

cmake_path(COMPARE "${wsl_SOURCE_DIR}" EQUAL "${wsl_BINARY_DIR}" BUILD_IN_SOURCE)
if (NOT ${BUILD_IN_SOURCE}) # Testing on 3.26 project_type_DIR paths appear canonicalized
    file(CREATE_LINK ${LLVM_INSTALL_DIR}/clang-format.exe ${wsl_SOURCE_DIR}/tools/clang-format.exe COPY_ON_ERROR)
endif()

set(LINUXSDK_PATH ${LINUXSDK_SOURCE_DIR}/${LLVM_ARCH})
set(LLVM_TARGET "${LLVM_ARCH}-unknown-linux-musl")
set(LINUX_CC ${LLVM_INSTALL_DIR}/clang.exe)
set(LINUX_CXX ${LLVM_INSTALL_DIR}/clang++.exe)
set(LINUX_AR ${LLVM_INSTALL_DIR}/llvm-ar.exe)
set(LINUX_COMMON_FLAGS --gcc-toolchain=${LINUXSDK_PATH}
                       -fpic
                       -B${LINUXSDK_PATH}
                       -isysroot ${LINUXSDK_PATH}
                       -isystem ${LINUXSDK_PATH}/include/c++/v1
                       -isystem ${LINUXSDK_PATH}/include
                       -isystem ${GSL_SOURCE_DIR}/include
                       -isystem "${WSLDEPS_SOURCE_DIR}/include/lxcore"
                       -isystem "${WSLDEPS_SOURCE_DIR}/include/schemas"
                       -I "${CMAKE_CURRENT_LIST_DIR}/src/linux/inc"
                       -I "${CMAKE_CURRENT_LIST_DIR}/src/linux/mountutil"
                       -I "${CMAKE_CURRENT_LIST_DIR}/src/linux/plan9"
                       -I "${CMAKE_CURRENT_LIST_DIR}/src/shared/configfile"
                       -I "${CMAKE_CURRENT_LIST_DIR}/src/shared/inc"
                       -I "${NLOHMAN_JSON_SOURCE_DIR}/include"
                       -I "${CMAKE_BINARY_DIR}/generated"
                       --no-standard-libraries
                       -Werror
                       -Wall
                       -Wpointer-arith
                       -D_POSIX_C_SOURCE=200809L
                       -Dswprintf_s=swprintf
                       -fms-extensions
                       -target ${LLVM_TARGET}
                       -D_GNU_SOURCE=1
                       -D_LARGEFILE64_SOURCE
                       -DWSL_PACKAGE_VERSION="${PACKAGE_VERSION}"
                       -DWSL_PACKAGE_VERSION_MAJOR=${PACKAGE_VERSION_MAJOR}
                       -DWSL_PACKAGE_VERSION_MINOR=${PACKAGE_VERSION_MINOR}
                       -DWSL_PACKAGE_VERSION_REVISION=${PACKAGE_VERSION_REVISION}
                       )

if (${TARGET_PLATFORM} STREQUAL "x64")
    set(LINUX_COMMON_FLAGS ${LINUX_COMMON_FLAGS} -D_AMD64_)
endif()

if (${TARGET_PLATFORM} STREQUAL "arm64")
    set(LINUX_COMMON_FLAGS ${LINUX_COMMON_FLAGS} -D_ARM64_)
endif()

set(LINUX_CXXFLAGS ${LINUX_COMMON_FLAGS} -std=c++20)
set(LINUX_CFLAGS ${LINUX_COMMON_FLAGS} -std=c99)

string(TOLOWER ${CMAKE_BUILD_TYPE} build_type)
if (build_type STREQUAL debug)
    set(LINUX_BUILD_TYPE_FLAGS -g3 -fno-inline-functions -DDEBUG -DDBG)
else()
    set(LINUX_BUILD_TYPE_FLAGS -g -O2 -DNDEBUG)
endif()

set(LINUX_LDFLAGS -target ${LLVM_TARGET}
                  --gcc-toolchain=${LINUXSDK_PATH}
                  -B${LINUXSDK_PATH}
                  -isysroot ${LINUXSDK_PATH}
                  -nostartfiles
                  --no-standard-libraries
                  -fuse-ld=lld.exe
                  -L${LINUXSDK_PATH}/lib
                  -L${LINUXSDK_PATH}/lib/linux
                  -L${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${CMAKE_BUILD_TYPE}
                  -lclang_rt.builtins-${LLVM_ARCH}
                  -l:libc.a
                  -static)

set(COMMON_LINUX_LINK_LIBRARIES configfile)

if(DEFINED ENV{WSL_DEV_BINARY_PATH})
    set(WSL_DEV_BINARY_PATH $ENV{WSL_DEV_BINARY_PATH})
endif()

if (DEFINED WSL_DEV_BINARY_PATH) # Development shortcut to make the package smaller
    add_compile_definitions(WSL_SYSTEM_DISTRO_PATH="${WSL_DEV_BINARY_PATH}/system.vhd"
                            WSL_KERNEL_PATH="${WSL_DEV_BINARY_PATH}/kernel"
                            WSL_KERNEL_MODULES_PATH="${WSL_DEV_BINARY_PATH}/modules.vhd"
                            WSL_DEV_INSTALL_PATH="${WSL_DEV_BINARY_PATH}"
                            WSL_GPU_LIB_PATH="${WSL_DEV_BINARY_PATH}/lib")
endif()

# Common include paths
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/wil/include)
include_directories(${WSLDEPS_SOURCE_DIR}/include)
include_directories(${WSLDEPS_SOURCE_DIR}/include/Windows)
include_directories(${WSLDEPS_SOURCE_DIR}/include/schemas)
include_directories(${WSLDEPS_SOURCE_DIR}/include/lxcore)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/src/shared/inc)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/src/windows/inc)
include_directories(${CMAKE_CURRENT_BINARY_DIR}/src/windows/service/inc/${TARGET_PLATFORM}/${CMAKE_BUILD_TYPE})
include_directories(${CMAKE_CURRENT_BINARY_DIR}/src/windows/wslinstaller/inc/${TARGET_PLATFORM}/${CMAKE_BUILD_TYPE})
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/src/linux/init/inc)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/src/windows/common)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/src/shared/configfile)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/localization)
include_directories(${CMAKE_BINARY_DIR}/generated)

include_directories(${WIL_SOURCE_DIR}/include)
include_directories(${GSL_SOURCE_DIR}/include)
include_directories(${TAEF_SOURCE_DIR}/build/include)

include_directories(${NLOHMAN_JSON_SOURCE_DIR}/include)
link_directories(${TAEF_SOURCE_DIR}/build/Library/${TARGET_PLATFORM})
set(TAEF_LINK_LIBRARIES
    TE.Common.lib
    Wex.Common.lib
    Wex.Logger.lib)

# Subprojects
add_subdirectory(nuget)
add_subdirectory(msixgluepackage)
add_subdirectory(msipackage)
add_subdirectory(msixinstaller)
add_subdirectory(src/windows/common)
add_subdirectory(src/windows/service)
add_subdirectory(src/windows/wslinstaller/inc)
add_subdirectory(src/windows/wslinstaller/stub)
add_subdirectory(src/windows/wslinstaller/exe)
add_subdirectory(src/shared/configfile)
add_subdirectory(src/windows/wsl)
add_subdirectory(src/windows/wslg)
add_subdirectory(src/windows/wslhost)
add_subdirectory(src/windows/wslrelay)
add_subdirectory(src/windows/wslinstall)

if (WSL_BUILD_WSL_SETTINGS)
    add_subdirectory(src/windows/libwsl)
    add_subdirectory(src/windows/wslsettings)
endif()

add_subdirectory(src/linux/netlinkutil)
add_subdirectory(src/linux/mountutil)
add_subdirectory(src/linux/plan9)
add_subdirectory(src/linux/init)
add_subdirectory(localization)

add_subdirectory(test/windows)

if (DEFINED PIPELINE_BUILD_ID)
    add_subdirectory(cloudtest)
endif()


if(DEFINED ENV{WSL_POST_BUILD_COMMAND})
    set(WSL_POST_BUILD_COMMAND $ENV{WSL_POST_BUILD_COMMAND})
endif ()

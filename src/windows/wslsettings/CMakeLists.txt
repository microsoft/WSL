set(TargetApp wslsettings)

project(${TargetApp} LANGUAGES CSharp)

# needed for csharp_set_xaml_cs_properties
include(CSharpUtilities)

# place wsl settings binaries/assets into
# separate directory to simplify packaging
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${TargetApp}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG ${CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG}/${TargetApp})
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE}/${TargetApp})

file(MAKE_DIRECTORY ${BIN}/${TargetApp}/Assets)

# Symlink the icon in the output directory to make local development easier
file(CREATE_LINK ${CMAKE_SOURCE_DIR}/images/wsl.ico ${BIN}/${TargetApp}/Assets/wsl.ico)
file(CREATE_LINK ${CMAKE_SOURCE_DIR}/images/wslbw.ico ${BIN}/${TargetApp}/Assets/wslbw.ico)
file(CREATE_LINK ${CMAKE_SOURCE_DIR}/images/SettingsOOBEDockerIcon.png ${BIN}/${TargetApp}/Assets/SettingsOOBEDockerIcon.png)
file(CREATE_LINK ${CMAKE_SOURCE_DIR}/images/SettingsOOBEFileExplorerIcon.png ${BIN}/${TargetApp}/Assets/SettingsOOBEFileExplorerIcon.png)
file(CREATE_LINK ${CMAKE_SOURCE_DIR}/images/SettingsOOBEVSCodeIcon.png ${BIN}/${TargetApp}/Assets/SettingsOOBEVSCodeIcon.png)

file(CREATE_LINK ${CMAKE_SOURCE_DIR}/images/SettingsOOBECrossOSFileAccess.gif ${BIN}/${TargetApp}/Assets/SettingsOOBECrossOSFileAccess.gif)
file(CREATE_LINK ${CMAKE_SOURCE_DIR}/images/SettingsOOBEDistroManagement.png ${BIN}/${TargetApp}/Assets/SettingsOOBEDistroManagement.png)
file(CREATE_LINK ${CMAKE_SOURCE_DIR}/images/SettingsOOBEDockerDesktopIntegration.png ${BIN}/${TargetApp}/Assets/SettingsOOBEDockerDesktopIntegration.png)
file(CREATE_LINK ${CMAKE_SOURCE_DIR}/images/SettingsOOBEGeneral.png ${BIN}/${TargetApp}/Assets/SettingsOOBEGeneral.png)
file(CREATE_LINK ${CMAKE_SOURCE_DIR}/images/SettingsOOBEGPUAcceleration.gif ${BIN}/${TargetApp}/Assets/SettingsOOBEGPUAcceleration.gif)
file(CREATE_LINK ${CMAKE_SOURCE_DIR}/images/SettingsOOBEGUIApps.png ${BIN}/${TargetApp}/Assets/SettingsOOBEGUIApps.png)
file(CREATE_LINK ${CMAKE_SOURCE_DIR}/images/SettingsOOBENetworkingIntegration.png ${BIN}/${TargetApp}/Assets/SettingsOOBENetworkingIntegration.png)
file(CREATE_LINK ${CMAKE_SOURCE_DIR}/images/SettingsOOBEVSCodeIntegration.png ${BIN}/${TargetApp}/Assets/SettingsOOBEVSCodeIntegration.png)

file(GLOB STRINGS ${CMAKE_SOURCE_DIR}/localization/strings/**/*.resw)

add_executable(
  ${TargetApp} WIN32
  app.manifest
  App.xaml
  App.xaml.cs
  Constants.cs
  LibWsl.cs
  Usings.cs
  Activation/ActivationHandler.cs
  Activation/DefaultActivationHandler.cs
  Activation/IActivationHandler.cs
  Activation/ProtocolActivationHandler.cs
  Behaviors/NavigationViewHeaderBehavior.cs
  Behaviors/NavigationViewHeaderMode.cs
  Contracts/Services/IActivationService.cs
  Contracts/Services/INavigationService.cs
  Contracts/Services/INavigationViewService.cs
  Contracts/Services/IPageService.cs
  Contracts/Services/IWindowService.cs
  Contracts/Services/IWslConfigService.cs
  Contracts/ViewModels/INavigationAware.cs
  Controls/HyperlinkTextBlock.xaml
  Controls/HyperlinkTextBlock.xaml.cs
  Controls/OOBEContent.xaml
  Controls/OOBEContent.xaml.cs
  Converters/BooleanToVisibilityConverter.cs
  Converters/MegabyteNumberConverter.cs
  Converters/MegabyteStringConverter.cs
  Converters/MillisecondsStringConverter.cs
  Helpers/FrameExtensions.cs
  Helpers/NavigationHelper.cs
  Helpers/ResourceExtensions.cs
  Helpers/RuntimeHelper.cs
  Helpers/TitleBarHelper.cs
  Properties/AssemblyInfo.cs
  Services/ActivationService.cs
  Services/NavigationService.cs
  Services/NavigationViewService.cs
  Services/PageService.cs
  Services/WindowService.cs
  Services/WslConfigService.cs
  Styles/Button.xaml
  Styles/FontSizes.xaml
  Styles/CommonStyles.xaml
  Styles/Thickness.xaml
  ViewModels/OOBE/DistroManagementViewModel.cs
  ViewModels/OOBE/DockerDesktopIntegrationViewModel.cs
  ViewModels/OOBE/GeneralViewModel.cs
  ViewModels/OOBE/GPUAccelerationViewModel.cs
  ViewModels/OOBE/GUIAppsViewModel.cs
  ViewModels/OOBE/NetworkingIntegrationViewModel.cs
  ViewModels/OOBE/VSCodeIntegrationViewModel.cs
  ViewModels/OOBE/WorkingAcrossFileSystemsViewModel.cs
  ViewModels/Settings/AboutViewModel.cs
  ViewModels/Settings/DeveloperViewModel.cs
  ViewModels/Settings/FileSystemViewModel.cs
  ViewModels/Settings/MemAndProcViewModel.cs
  ViewModels/Settings/NetworkingViewModel.cs
  ViewModels/Settings/OptionalFeaturesViewModel.cs
  ViewModels/Settings/WslConfigSettingViewModel.cs
  ViewModels/ShellViewModel.cs
  Views/OOBE/DistroManagementPage.xaml
  Views/OOBE/DistroManagementPage.xaml.cs
  Views/OOBE/DockerDesktopIntegrationPage.xaml
  Views/OOBE/DockerDesktopIntegrationPage.xaml.cs
  Views/OOBE/GeneralPage.xaml
  Views/OOBE/GeneralPage.xaml.cs
  Views/OOBE/GPUAccelerationPage.xaml
  Views/OOBE/GPUAccelerationPage.xaml.cs
  Views/OOBE/GUIAppsPage.xaml
  Views/OOBE/GUIAppsPage.xaml.cs
  Views/OOBE/NetworkingIntegrationPage.xaml
  Views/OOBE/NetworkingIntegrationPage.xaml.cs
  Views/OOBE/ShellPage.xaml
  Views/OOBE/ShellPage.xaml.cs
  Views/OOBE/VSCodeIntegrationPage.xaml
  Views/OOBE/VSCodeIntegrationPage.xaml.cs
  Views/OOBE/WorkingAcrossFileSystemsPage.xaml
  Views/OOBE/WorkingAcrossFileSystemsPage.xaml.cs
  Views/Settings/AboutPage.xaml
  Views/Settings/AboutPage.xaml.cs
  Views/Settings/DeveloperPage.xaml
  Views/Settings/DeveloperPage.xaml.cs
  Views/Settings/FileSystemPage.xaml
  Views/Settings/FileSystemPage.xaml.cs
  Views/Settings/MemAndProcPage.xaml
  Views/Settings/MemAndProcPage.xaml.cs
  Views/Settings/NetworkingPage.xaml
  Views/Settings/NetworkingPage.xaml.cs
  Views/Settings/OptionalFeaturesPage.xaml
  Views/Settings/OptionalFeaturesPage.xaml.cs
  Views/Settings/ShellPage.xaml
  Views/Settings/ShellPage.xaml.cs
  Windows/OOBEWindow.xaml
  Windows/OOBEWindow.xaml.cs
  Windows/MainWindow.xaml
  Windows/MainWindow.xaml.cs
  ${STRINGS}
)

csharp_set_xaml_cs_properties(
  Controls/HyperlinkTextBlock.xaml
  Controls/HyperlinkTextBlock.xaml.cs
  Styles/Button.xaml
  Styles/FontSizes.xaml
  Styles/CommonStyles.xaml
  Styles/Thickness.xaml
  Windows/OOBEWindow.xaml
  Windows/OOBEWindow.xaml.cs
  Windows/MainWindow.xaml
  Windows/MainWindow.xaml.cs
  Views/OOBE/DistroManagementPage.xaml
  Views/OOBE/DistroManagementPage.xaml.cs
  Views/OOBE/DockerDesktopIntegrationPage.xaml
  Views/OOBE/DockerDesktopIntegrationPage.xaml.cs
  Views/OOBE/GeneralPage.xaml
  Views/OOBE/GeneralPage.xaml.cs
  Views/OOBE/GPUAccelerationPage.xaml
  Views/OOBE/GPUAccelerationPage.xaml.cs
  Views/OOBE/GUIAppsPage.xaml
  Views/OOBE/GUIAppsPage.xaml.cs
  Views/OOBE/NetworkingIntegrationPage.xaml
  Views/OOBE/NetworkingIntegrationPage.xaml.cs
  Views/OOBE/ShellPage.xaml
  Views/OOBE/ShellPage.xaml.cs
  Views/OOBE/VSCodeIntegrationPage.xaml
  Views/OOBE/VSCodeIntegrationPage.xaml.cs
  Views/OOBE/WorkingAcrossFileSystemsPage.xaml
  Views/OOBE/WorkingAcrossFileSystemsPage.xaml.cs
  Views/Settings/AboutPage.xaml
  Views/Settings/AboutPage.xaml.cs
  Views/Settings/DeveloperPage.xaml
  Views/Settings/DeveloperPage.xaml.cs
  Views/Settings/FileSystemPage.xaml
  Views/Settings/FileSystemPage.xaml.cs
  Views/Settings/MemAndProcPage.xaml
  Views/Settings/MemAndProcPage.xaml.cs
  Views/Settings/NetworkingPage.xaml
  Views/Settings/NetworkingPage.xaml.cs
  Views/Settings/OptionalFeaturesPage.xaml
  Views/Settings/OptionalFeaturesPage.xaml.cs
  Views/Settings/ShellPage.xaml
  Views/Settings/ShellPage.xaml.cs
)

set_property(
  SOURCE App.xaml
  PROPERTY VS_XAML_TYPE
  "ApplicationDefinition"
)

# Set the C# language version (defaults to 3.0).
set(
  CMAKE_CSharp_FLAGS
  "/langversion:latest"
)

target_compile_options(
  ${TargetApp}
  PRIVATE "/debug:full"
  PRIVATE "/unsafe"
)

if (${OFFICIAL_BUILD})
    add_compile_definitions(WSL_OFFICIAL_BUILD TELEMETRYEVENTSOURCE_PUBLIC)
endif()

# Package References and versions per WinUI Template Studio Sample app
set_property(
  TARGET ${TargetApp}
  PROPERTY VS_PACKAGE_REFERENCES
  "CommunityToolkit.Mvvm_${CTK_MVVM_VERSION};\
CommunityToolkit.WinUI.Animations_${CTK_ANIMATIONS_VERSION};\
CommunityToolkit.WinUI.Controls.SettingsControls_${CTK_STTNGS_CTRLS_VERSION};\
Microsoft.Extensions.Hosting_${EXTS_HOSTING_VERSION};\
Microsoft.WindowsAppSDK_${WIN_APP_SDK_VERSION};\
Microsoft.Xaml.Behaviors.WinUI.Managed_${XAML_BEHAVIORS_VERSION};\
WinUIEx_${WINUIEX_VERSION}"
)

set(
  TARGET_PLATFORM_VERSION
  "10.0.26100.0"
)
set(
  WINDOWS_TARGET_PLATFORM_VERSION
  "windows${TARGET_PLATFORM_VERSION}"
)
set(
  TARGET_PLATFORM_MIN_VERSION
  "10.0.19041.0"
)
set(
  WINDOWS_TARGET_PLATFORM_MIN_VERSION
  "windows${TARGET_PLATFORM_MIN_VERSION}"
)

set_target_properties(
  ${TargetApp} PROPERTIES
  # ----- Dotnet, Windows App SDK and WinUI stuff starts here -----
  VS_GLOBAL_RootNamespace wslsettings
  VS_GLOBAL_AppContainerApplication false
  VS_GLOBAL_AppxPackage false
  VS_GLOBAL_UseWinUI true
  VS_GLOBAL_WindowsPackageType None
  VS_GLOBAL_SelfContained true
  VS_GLOBAL_WindowsAppSDKSelfContained true
  VS_GLOBAL_ApplicationIcon "${CMAKE_SOURCE_DIR}/images/wsl.ico"
  VS_GLOBAL_ApplicationManifest "${CMAKE_CURRENT_SOURCE_DIR}/app.manifest"
  VS_GLOBAL_RuntimeIdentifier "win-${TARGET_PLATFORM}"
  VS_GLOBAL_RuntimeIdentifiers "win-${TARGET_PLATFORM}"
  VS_GLOBAL_Platform "${TARGET_PLATFORM}"
  VS_GLOBAL_Platforms "${TARGET_PLATFORM}"
  VS_GLOBAL_PlatformTarget "${TARGET_PLATFORM}"
  VS_GLOBAL_TargetPlatformVersion "${TARGET_PLATFORM_VERSION}"
  VS_GLOBAL_TargetPlatformMinVersion "${TARGET_PLATFORM_MIN_VERSION}"
  VS_GLOBAL_WindowsSdkPackageVersion "${WINDOWS_SDK_DOTNET_VERSION}"
  VS_GLOBAL_ImplicitUsings enable
  VS_GLOBAL_Nullable enable
  VS_GLOBAL_AppendRuntimeIdentifierToOutputPath false
  VS_GLOBAL_GenerateAssemblyInfo false
  VS_GLOBAL_TargetLatestRuntimePatch false
  DOTNET_SDK "Microsoft.NET.Sdk"
  DOTNET_TARGET_FRAMEWORK "net8.0-${WINDOWS_TARGET_PLATFORM_VERSION}"
)

configure_file(
  "${CMAKE_CURRENT_LIST_DIR}/directory.build.targets.in"
  "${CMAKE_CURRENT_LIST_DIR}/directory.build.targets"
)

configure_file(
  "${CMAKE_CURRENT_LIST_DIR}/Properties/AssemblyInfo.cs.in"
  "${CMAKE_CURRENT_LIST_DIR}/Properties/AssemblyInfo.cs"
)

add_dependencies(wslsettings libwsl)
set_target_properties(wslsettings PROPERTIES FOLDER windows)
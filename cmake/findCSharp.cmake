function(configure_csharp_target TARGET)
    project(${TARGET} LANGUAGES CSharp)

    # Set the C# language version (defaults to 3.0).
    set(CMAKE_CSharp_FLAGS "/langversion:latest")

    target_compile_options(
        ${TARGET}
        PRIVATE "/debug:full"
    )

    set(TARGET_PLATFORM_VERSION "10.0.26100.0")
    set(WINDOWS_TARGET_PLATFORM_VERSION "windows${TARGET_PLATFORM_VERSION}")
    set(TARGET_PLATFORM_MIN_VERSION "10.0.19041.0")
    set(WINDOWS_TARGET_PLATFORM_MIN_VERSION "windows${TARGET_PLATFORM_MIN_VERSION}")

    set_target_properties(
    ${TARGET} PROPERTIES
    VS_GLOBAL_TargetPlatformVersion "${TARGET_PLATFORM_VERSION}"
    VS_GLOBAL_TargetPlatformMinVersion "${TARGET_PLATFORM_MIN_VERSION}"
    VS_GLOBAL_WindowsSdkPackageVersion "${WINDOWS_SDK_DOTNET_VERSION}"
    VS_GLOBAL_AppendRuntimeIdentifierToOutputPath false
    VS_GLOBAL_GenerateAssemblyInfo false
    VS_GLOBAL_TargetLatestRuntimePatch false
    DOTNET_SDK "Microsoft.NET.Sdk"
    DOTNET_TARGET_FRAMEWORK "net8.0-${WINDOWS_TARGET_PLATFORM_VERSION}"
    )
endfunction()
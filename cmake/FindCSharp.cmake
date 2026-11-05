function(configure_csharp_target TARGET FRAMEWORK)
    set(TARGET_PLATFORM_MIN_VERSION "10.0.19041.0")

    target_compile_options(
        ${TARGET}
        PRIVATE
            "/langversion:latest"
            "/debug:full")

    set_target_properties(
        ${TARGET} PROPERTIES
        VS_GLOBAL_TargetPlatformVersion "${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}"
        VS_GLOBAL_TargetPlatformMinVersion "${TARGET_PLATFORM_MIN_VERSION}"
        VS_GLOBAL_WindowsSdkPackageVersion "${WINDOWS_SDK_DOTNET_VERSION}"
        VS_GLOBAL_AppendRuntimeIdentifierToOutputPath false
        VS_GLOBAL_TargetLatestRuntimePatch false
        DOTNET_SDK "Microsoft.NET.Sdk"
        DOTNET_TARGET_FRAMEWORK "${FRAMEWORK}-windows${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}"
    )
endfunction()
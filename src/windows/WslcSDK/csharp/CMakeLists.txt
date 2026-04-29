enable_language(CSharp)

add_library(wslcsdkcs SHARED)
configure_csharp_target(wslcsdkcs)

set(WSLCSDK_WINMD "${CMAKE_CURRENT_BINARY_DIR}/../winrt/${TARGET_PLATFORM}/${CMAKE_BUILD_TYPE}/wslcsdk.winmd")

target_sources(wslcsdkcs
  PRIVATE
  "${WSLCSDK_WINMD}"
  "${CMAKE_CURRENT_LIST_DIR}/WinRTActivation.cs"
)

set_source_files_properties(
  "${WSLCSDK_WINMD}"
  PROPERTIES
  GENERATED TRUE
  VS_TOOL_OVERRIDE "CsWinRTInputs"
)

set_target_properties(
  wslcsdkcs PROPERTIES
  FOLDER windows
  LINKER_LANGUAGE CSharp
  VS_PACKAGE_REFERENCES "Microsoft.Windows.CsWinRT_${CSWINRT_VERSION}"
  VS_GLOBAL_CsWinRTIncludes "Microsoft.WSL.Containers"
)

add_dependencies(wslcsdkcs wslcsdkwinrt)

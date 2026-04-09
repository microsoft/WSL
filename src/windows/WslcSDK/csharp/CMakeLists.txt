project(wslcsdkcs LANGUAGES CSharp)

add_library(wslcsdkcs SHARED "Projection.cs")
configure_csharp_target(wslcsdkcs)

set_target_properties(
  wslcsdkcs PROPERTIES
  FOLDER windows
  LINKER_LANGUAGE CSharp
  VS_GLOBAL_PlatformTarget "AnyCPU"
)

add_dependencies(wslcsdkcs wslcsdk)

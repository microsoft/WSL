add_idl(wslserviceidl "wslservice.idl" "windowsdefs.idl")
add_idl(wslpluginhostidl "WslPluginHost.idl" "")
set_target_properties(wslserviceidl PROPERTIES FOLDER windows)
set_target_properties(wslpluginhostidl PROPERTIES FOLDER windows)
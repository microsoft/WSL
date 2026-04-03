add_idl(wslserviceidl "wslservice.idl" "windowsdefs.idl")
add_idl(wslcidl "wslc.idl" "")
set_target_properties(wslserviceidl PROPERTIES FOLDER windows)
set_target_properties(wslcidl PROPERTIES FOLDER windows)
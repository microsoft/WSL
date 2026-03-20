add_idl(wslserviceidl "wslservice.idl" "windowsdefs.idl")
add_idl(wslaserviceidl "wslaservice.idl" "")
set_target_properties(wslserviceidl PROPERTIES FOLDER windows)
set_target_properties(wslaserviceidl PROPERTIES FOLDER windows)
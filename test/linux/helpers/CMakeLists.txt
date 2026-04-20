set(SOURCES port_rebind_test.c)

add_linux_executable(port_rebind_test "${SOURCES}" "" "")

set_target_properties(port_rebind_test PROPERTIES FOLDER test/linux/helpers)

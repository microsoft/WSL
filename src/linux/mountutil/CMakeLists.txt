set(SOURCES
    mountflags.cpp
    mountutil.c)

set(HEADERS
    mountutil.h
    mountutilcpp.h)

add_linux_library(libmountutil "${SOURCES}" "${HEADERS}")
set_target_properties(libmountutil PROPERTIES FOLDER linux)
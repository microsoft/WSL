set(SOURCES
    p9fid.cpp
    p9file.cpp
    p9fs.cpp
    p9handler.cpp
    p9io.cpp
    p9lx.cpp
    p9readdir.cpp
    p9scheduler.cpp
    p9tracelogging.cpp
    p9util.cpp
    p9xattr.cpp)

set(HEADERS
    p9fid.h
    p9file.h
    p9fs.h
    p9handler.h
    p9io.h
    p9lx.h
    p9readdir.h
    p9scheduler.h
    p9tracelogging.h
    p9tracelogginghelper.h
    p9util.h
    p9xattr.h
    p9defs.h
    p9protohelpers.h
    p9await.h
    p9errors.h
    result_macros.h
    expected.h
    p9platform.h
    p9ihandler.h
    p9log.h
    p9data.h
    p9commonutil.h
    precomp.h)

add_linux_library(libplan9 "${SOURCES}" "${HEADERS}")
set_target_properties(libplan9 PROPERTIES FOLDER linux)
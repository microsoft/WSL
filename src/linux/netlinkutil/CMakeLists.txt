set(SOURCES
    Address.cpp
    Interface.cpp
    IpNeighborManager.cpp
    IpRuleManager.cpp
    Neighbor.cpp
    NetlinkError.cpp
    NetlinkParseException.cpp
    NetlinkResponse.cpp
    NetlinkTransaction.cpp
    NetlinkTransactionError.cpp
    Route.cpp
    RoutingTable.cpp
    Rule.cpp
    RuntimeErrorWithSourceLocation.cpp
    SyscallError.cpp
    Utils.cpp)

set(HEADERS
    address.h
    Forwarder.h
    Fwd.h
    Interface.h
    InterfaceConfiguration.h
    IpNeighborManager.h
    IpRuleManager.h
    Neighbor.h
    NetlinkError.h
    NetlinkChannel.h
    NetlinkMessage.h
    NetlinkParseException.h
    NetlinkResponse.h
    NetlinkTransaction.h
    NetlinkTransactionError.h
    Packet.h
    Protocol.h
    Operation.h
    Route.h
    RoutingTable.h
    Rule.h
    RuntimeErrorWithSourceLocation.h
    Syscall.h
    SyscallError.h
    Utils.h
    Forwarder.hxx
    NetlinkChannel.hxx
    NetlinkMessage.hxx
    NetlinkResponse.hxx
    NetLinkStrings.h
    Syscall.hxx
    Utils.hxx)

set(LINUX_CXXFLAGS ${LINUX_CXXFLAGS} -I "${CMAKE_CURRENT_LIST_DIR}/../init")

add_linux_library(libnetlinkutil "${SOURCES}" "${HEADERS}")
set_target_properties(libnetlinkutil PROPERTIES FOLDER linux)

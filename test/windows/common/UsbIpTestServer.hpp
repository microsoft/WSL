// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include "UsbIpDevices.hpp"
#include <memory>
#include <string>

namespace wsl::test::usbip {

class Server
{
public:
    Server();
    ~Server();
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    uint16_t Port() const;
    std::wstring SerialNumber() const;
    std::wstring InstanceId(Device device) const;

    std::string ReserveImport(Device device) const;
    void WithdrawImport(Device device, std::string_view busId) const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace wsl::test::usbip
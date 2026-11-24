// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include "UsbIpTestServer.hpp"
#include <filesystem>
#include <optional>

namespace wsl::test::usbip {

std::optional<std::filesystem::path> FindClient();

class ScopedImport
{
public:
    ScopedImport(const std::filesystem::path& cli, const Server& server, Device device);
    ~ScopedImport();
    ScopedImport(const ScopedImport&) = delete;
    ScopedImport& operator=(const ScopedImport&) = delete;

private:
    void Detach() const;

    std::filesystem::path m_cli;
    const Server& m_server;
    Device m_device;
    std::string m_busId;
};

} // namespace wsl::test::usbip
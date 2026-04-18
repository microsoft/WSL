/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    VersionModel.h

Abstract:

    This file contains the VersionModel definitions
--*/

#pragma once

namespace wsl::windows::wslc::models {

#define WSLC_VERSION_ROW(key, value) std::format("{:<20}{}", key ":", value)

struct ClientVersion
{
    const std::string Version = WSL_PACKAGE_VERSION;
    const std::string GitCommit = COMMIT_HASH;
    const std::string Built = __DATE__ " " __TIME__;
    const std::string Os = "windows";

#if defined(_AMD64_)
    const std::string Arch = "amd64";
#elif defined(_ARM64_)
    const std::string Arch = "arm64";
#else
    const std::string Arch = "unknown";
#endif

    NLOHMANN_DEFINE_TYPE_INTRUSIVE(ClientVersion, Version, GitCommit, Built, Os, Arch);

    std::string ToString() const
    {
        std::stringstream ss;
        ss << "Client:" << std::endl;
        ss << WSLC_VERSION_ROW("  Version", Version) << std::endl;
        ss << WSLC_VERSION_ROW("  Git commit", GitCommit) << std::endl;
        ss << WSLC_VERSION_ROW("  Built", Built) << std::endl;
        ss << WSLC_VERSION_ROW("  OS/Arch", Os + "/" + Arch) << std::endl;
        return ss.str();
    }
};

struct ServerVersion
{
    const std::string Kernel = KERNEL_VERSION;
    const std::string WSLg = WSLG_VERSION;
    const std::string MSRDC = MSRDC_VERSION;
    const std::string Direct3D = DIRECT3D_VERSION;
    const std::string DXCore = DXCORE_VERSION;
    const std::string Windows = wsl::windows::common::helpers::GetWindowsVersionString();

    NLOHMANN_DEFINE_TYPE_INTRUSIVE(ServerVersion, Kernel, WSLg, MSRDC, Direct3D, DXCore, Windows);

    std::string ToString() const
    {
        std::stringstream ss;
        ss << "Server:" << std::endl;
        ss << WSLC_VERSION_ROW("  Linux kernel", Kernel) << std::endl;
        ss << WSLC_VERSION_ROW("  WSLg", WSLg) << std::endl;
        ss << WSLC_VERSION_ROW("  MSRDC", MSRDC) << std::endl;
        ss << WSLC_VERSION_ROW("  Direct3D", Direct3D) << std::endl;
        ss << WSLC_VERSION_ROW("  DXCore", DXCore) << std::endl;
        ss << WSLC_VERSION_ROW("  Windows", Windows) << std::endl;
        return ss.str();
    }
};

struct VersionInfo
{
    ClientVersion Client;
    ServerVersion Server;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE(VersionInfo, Client, Server);

    std::string ToString() const
    {
        std::stringstream ss;
        ss << Client.ToString() << std::endl;
        ss << Server.ToString();
        return ss.str();
    }
};
#undef WSLC_VERSION_ROW
} // namespace wsl::windows::wslc::models

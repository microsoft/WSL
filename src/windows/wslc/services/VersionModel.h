/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    VersionModel.h

Abstract:

    This file contains the VersionModel definitions
--*/

#pragma once

namespace wsl::windows::wslc::models {

#define WSLC_VERSION_ROW(key, value) std::format("{:<20}{}\n", key ":", value)

struct ClientVersion
{
    std::string Version = WSL_PACKAGE_VERSION;
    std::string GitCommit = COMMIT_HASH;
    std::string Built = __DATE__ " " __TIME__;
    std::string Os = "windows";

#if defined(_AMD64_)
    std::string Arch = "amd64";
#elif defined(_ARM64_)
    std::string Arch = "arm64";
#else
    std::string Arch = "unknown";
#endif

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(ClientVersion, Version, GitCommit, Built, Os, Arch);

    std::string ToString() const
    {
        std::stringstream ss;
        ss << "Client:\n";
        ss << WSLC_VERSION_ROW("  Version", Version);
        ss << WSLC_VERSION_ROW("  Git commit", GitCommit);
        ss << WSLC_VERSION_ROW("  Built", Built);
        ss << WSLC_VERSION_ROW("  OS/Arch", Os + "/" + Arch);
        return ss.str();
    }
};

struct ServerVersion
{
    std::string Kernel = KERNEL_VERSION;
    std::string WSLg = WSLG_VERSION;
    std::string MSRDC = MSRDC_VERSION;
    std::string Direct3D = DIRECT3D_VERSION;
    std::string DXCore = DXCORE_VERSION;
    std::string Windows = wsl::windows::common::helpers::GetWindowsVersionString();

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(ServerVersion, Kernel, WSLg, MSRDC, Direct3D, DXCore, Windows);

    std::string ToString() const
    {
        std::stringstream ss;
        ss << "Server:\n";
        ss << WSLC_VERSION_ROW("  Linux kernel", Kernel);
        ss << WSLC_VERSION_ROW("  WSLg", WSLg);
        ss << WSLC_VERSION_ROW("  MSRDC", MSRDC);
        ss << WSLC_VERSION_ROW("  Direct3D", Direct3D);
        ss << WSLC_VERSION_ROW("  DXCore", DXCore);
        ss << WSLC_VERSION_ROW("  Windows", Windows);
        return ss.str();
    }
};

struct VersionInfo
{
    ClientVersion Client{};
    ServerVersion Server{};

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(VersionInfo, Client, Server);

    std::string ToString() const
    {
        std::stringstream ss;
        ss << Client.ToString() << "\n";
        ss << Server.ToString();
        return ss.str();
    }
};
#undef WSLC_VERSION_ROW
} // namespace wsl::windows::wslc::models

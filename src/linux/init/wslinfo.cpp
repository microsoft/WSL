/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    wslinfo.cpp

Abstract:

    This file wslpath function definitions.

--*/

#include "common.h"
#include <iostream>
#include <assert.h>
#include "getopt.h"
#include "util.h"
#include "wslpath.h"
#include "wslinfo.h"
#include "lxinitshared.h"
#include "defs.h"
#include "Localization.h"
#include "CommandLine.h"
#include "../../shared/inc/lxinitshared.h"

enum class WslInfoMode
{
    GetNetworkingMode,
    MsalProxyPath,
    WslVersion,
    VMId
};

int WslInfoEntry(int Argc, char* Argv[])

/*++

Routine Description:

    This routine is the entrypoint for the wslinfo binary.

Arguments:

    Argc - Supplies the argument count.

    Argv - Supplies the command line arguments.

Return Value:

    0 on success, 1 on failure.

--*/

{
    using namespace wsl::shared;

    constexpr auto Usage = std::bind(Localization::MessageWslInfoUsage, Localization::Options::Default);

    std::optional<WslInfoMode> Mode;
    bool noNewLine = false;

    ArgumentParser parser(Argc, Argv);

    parser.AddArgument(UniqueSetValue<WslInfoMode, WslInfoMode::GetNetworkingMode>{Mode, Usage}, WSLINFO_NETWORKING_MODE);
    parser.AddArgument(UniqueSetValue<WslInfoMode, WslInfoMode::MsalProxyPath>{Mode, Usage}, WSLINFO_MSAL_PROXY_PATH);
    parser.AddArgument(UniqueSetValue<WslInfoMode, WslInfoMode::WslVersion>{Mode, Usage}, WSLINFO_WSL_VERSION);
    parser.AddArgument(UniqueSetValue<WslInfoMode, WslInfoMode::WslVersion>{Mode, Usage}, WSLINFO_WSL_VERSION_LEGACY);
    parser.AddArgument(UniqueSetValue<WslInfoMode, WslInfoMode::VMId>{Mode, Usage}, WSLINFO_WSL_VMID);
    parser.AddArgument(NoOp{}, WSLINFO_WSL_HELP);
    parser.AddArgument(noNewLine, nullptr, WSLINFO_NO_NEWLINE);

    try
    {
        parser.Parse();
    }
    catch (const wil::ExceptionWithUserMessage& e)
    {
        std::cerr << e.what() << "\n";
        return 1;
    }

    if (!Mode.has_value())
    {
        std::cerr << Usage() << "\n";
        return 1;
    }
    else if (Mode.value() == WslInfoMode::GetNetworkingMode)
    {
        if (UtilIsUtilityVm())
        {
            auto NetworkingMode = UtilGetNetworkingMode();
            if (!NetworkingMode)
            {
                std::cerr << Localization::MessageFailedToQueryNetworkingMode() << "\n";
                return 1;
            }

            switch (NetworkingMode.value())
            {
            case LxMiniInitNetworkingModeNat:
                std::cout << "nat";
                break;

            case LxMiniInitNetworkingModeBridged:
                std::cout << "bridged";
                break;

            case LxMiniInitNetworkingModeMirrored:
                std::cout << "mirrored";
                break;

            case LxMiniInitNetworkingModeVirtioProxy:
                std::cout << "virtioproxy";
                break;

            default:
                std::cout << "none";
                break;
            }
        }
        else
        {
            std::cout << "wsl1";
        }
    }
    else if (Mode.value() == WslInfoMode::MsalProxyPath)
    {
        auto value = UtilGetEnvironmentVariable(LX_WSL2_INSTALL_PATH);
        if (value.empty())
        {
            std::cerr << Localization::MessageNoValueFound() << "\n";
            return 1;
        }

        auto translatedPath = WslPathTranslate(value.data(), TRANSLATE_FLAG_ABSOLUTE, TRANSLATE_MODE_UNIX);
        if (translatedPath.empty())
        {
            std::cerr << Localization::MessageFailedToTranslate(value.data()) << "\n";
            return 1;
        }

        std::cout << translatedPath << "/msal.wsl.proxy.exe";
    }
    else if (Mode.value() == WslInfoMode::WslVersion)
    {
        std::cout << WSL_PACKAGE_VERSION;
    }
    else if (Mode.value() == WslInfoMode::VMId)
    {
        if (UtilIsUtilityVm())
        {
            auto vmId = UtilGetVmId();
            if (vmId.empty())
            {
                std::cerr << Localization::MessageNoValueFound() << "\n";
                return 1;
            }

            std::cout << vmId;
        }
        else
        {
            std::cout << "wsl1";
        }
    }
    else
    {
        assert(false && "Unknown WslInfoMode");
        return 1;
    }

    if (!noNewLine)
    {
        std::cout << '\n';
    }

    return 0;
}

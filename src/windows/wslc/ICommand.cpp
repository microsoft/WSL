/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ICommand.cpp

Abstract:

    This file contains the ICommand implementation

--*/
#include "precomp.h"
#include "ICommand.h"

namespace wslc::commands {
using namespace wsl::shared;
namespace wslutil = wsl::windows::common::wslutil;

std::string ICommand::GetFullDescription() const
{
    std::stringstream ss;
    ss << Description() << std::endl;
    for (const auto& option : Options())
    {
        ss << "  " << option << std::endl;
    }
    ss << "  -h, --help: Print this help message" << std::endl;
    return ss.str();
}

std::string ICommand::GetShortDescription() const
{
    return std::format("{}: {}", Name(), Description());
}

void ICommand::PrintHelp() const
{
    wslutil::PrintMessage(wsl::shared::string::MultiByteToWide(GetFullDescription()));
}
} // namespace wslc::commands
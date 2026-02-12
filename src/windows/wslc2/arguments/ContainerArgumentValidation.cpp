// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#pragma once
#include "pch.h"
#include "ArgumentTypes.h"
#include "Exceptions.h"

using namespace wsl::windows::wslc;
using namespace wsl::windows::wslc::argument;

namespace wsl::windows::wslc::validation
{
    // This is mostly just a placeholder validation to show how to set up specific arg validation.
    // This Publish validate is likely not complete or as thorough as we need, it's just a stand-in.
    void ValidatePublish([[maybe_unused]]const ArgType argType, const ArgMap& execArgs)
    {
        const auto& publishArgs = execArgs.GetAll<ArgType::Publish>();

        for (const auto& publishArg : publishArgs)
        {
            // Basic validation: ensure the format is "hostPort:containerPort"
            auto colonPos = publishArg.find(L':');
            if (colonPos == std::wstring::npos || colonPos == 0 || colonPos == publishArg.length() - 1)
            {
                throw ArgumentException(L"Localization::WSLCCLI_InvalidPublishFormatError(publishArg) " + publishArg);
            }

            auto hostPortStr = publishArg.substr(0, colonPos);
            auto containerPortStr = publishArg.substr(colonPos + 1);

            // Validate that both hostPort and containerPort are valid integers
            try
            {
                auto hostPort = std::stoi(std::wstring(hostPortStr));
                auto containerPort = std::stoi(std::wstring(containerPortStr));
                if (hostPort <= 0 || hostPort > 65535 || containerPort <= 0 || containerPort > 65535)
                {
                    throw ArgumentException(L"Localization::WSLCCLI_PortOutOfRangeError(publishArg) " + publishArg);
                }
            }
            catch (const std::exception&)
            {
                throw ArgumentException(L"Localization::WSLCCLI_InvalidPublishFormatError(publishArg) " + publishArg);
            }
        }
    }
}
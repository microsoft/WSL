/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ImageModel.cpp

Abstract:

    This file contains the ImageModel implementation.

--*/

#include "precomp.h"
#include "ImageModel.h"

namespace wsl::windows::wslc::models
{
RepoTag RepoTag::Parse(const std::string& repoTag)
{
    auto lastSlashPos = repoTag.rfind('/');
    auto lastColonPos = repoTag.rfind(':');

    if (lastSlashPos == std::string::npos)
    {
        if (lastColonPos == std::string::npos)
        {
            // Not slash and no colon (e.g. "debian")
            return { repoTag, "" };
        }

        // No slash but has colon (e.g. "debian:latest")
        return { repoTag.substr(0, lastColonPos), repoTag.substr(lastColonPos + 1) };
    }

    if (lastColonPos != std::string::npos && lastColonPos > lastSlashPos)
    {
        // Has slash and has colon after the last slash
        // (e.g. "myrepo/debian:latest")
        return { repoTag.substr(0, lastColonPos), repoTag.substr(lastColonPos + 1) };
    }

    // Has slash but no colon or colon before the last slash
    // (e.g. "myrepo/debian" or "myrepo:5000/debian")
    return { repoTag, "" };
}
} // namespace wsl::windows::wslc::models
/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ImageService.h

Abstract:

    This file contains the ImageService definition

--*/
#pragma once

#include "SessionModel.h"
#include "ImageModel.h"

namespace wsl::windows::wslc::services {
class ImageService
{
public:
    static std::vector<wsl::windows::wslc::models::ImageInformation> List(wsl::windows::wslc::models::Session& session);
    static void Pull(wsl::windows::wslc::models::Session& session, const std::string& image, IProgressCallback* callback);
    static void Load(wsl::windows::wslc::models::Session& session, const std::string& input);
    static void Load(wsl::windows::wslc::models::Session& session, const wil::unique_hfile& imageFile);
    void Push();
    void Save();
    void Tag();
    void Prune();
    void Inspect();
};
} // namespace wsl::windows::wslc::services

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
#include <docker_schema.h>

namespace wsl::windows::wslc::services {
class ImageService
{
public:
    static void Build(
        wsl::windows::wslc::models::Session& session,
        const std::wstring& contextPath,
        const std::wstring& tag,
        const std::wstring& dockerfilePath,
        IProgressCallback* callback);
    static std::vector<wsl::windows::wslc::models::ImageInformation> List(wsl::windows::wslc::models::Session& session);
    static void Load(wsl::windows::wslc::models::Session& session, const std::wstring& input);
    static std::vector<WSLADeletedImageInformation> Delete(wsl::windows::wslc::models::Session& session, const std::string& image, bool force, bool noPrune);
    static wsl::windows::common::docker_schema::InspectImage Inspect(wsl::windows::wslc::models::Session& session, const std::string& image);
    static void Pull(wsl::windows::wslc::models::Session& session, const std::string& image, IProgressCallback* callback);
    void Push();
    void Save();
    void Tag();
    void Prune();
};
} // namespace wsl::windows::wslc::services

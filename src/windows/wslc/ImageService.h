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

namespace wslc::services {
class ImageService
{
public:
    std::vector<wslc::models::ImageInformation> List(wslc::models::Session& session);
    void Pull(wslc::models::Session& session, const std::string& image, IProgressCallback* callback);
    void Push();
    void Save();
    void Load();
    void Tag();
    void Prune();
    void Inspect();
};
} // namespace wslc::services

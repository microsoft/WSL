/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ImageService.cpp

Abstract:

    This file contains the ImageService implementation

--*/
#include "ImageService.h"
#include "Utils.h"
#include "SessionService.h"
#include <wslutil.h>

namespace wslc::services
{

using namespace wslc::models;

std::vector<ImageInformation> ImageService::List(wslc::models::Session& session)
{
    wil::unique_cotaskmem_array_ptr<WSLA_IMAGE_INFORMATION> images;
    ULONG count = 0;
    THROW_IF_FAILED(session.Get()->ListImages(&images, &count));

    std::vector<ImageInformation> result;
    for (auto ptr = images.get(), end = images.get() + count; ptr != end; ++ptr)
    {
        const WSLA_IMAGE_INFORMATION& image = *ptr;
        ImageInformation info;
        info.Name = image.Image;
        info.Size = image.Size;
        result.push_back(info);
    }

    return result;
}

void ImageService::Pull(wslc::models::Session& session, const std::string& image, IProgressCallback* callback)
{
    THROW_IF_FAILED(session.Get()->PullImage(image.c_str(), nullptr, callback));
}

void ImageService::Push()
{
}

void ImageService::Save()
{
}

void ImageService::Load()
{
}

void ImageService::Tag()
{
}

void ImageService::Prune()
{
}

void ImageService::Inspect()
{
}

}

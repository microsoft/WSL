#include "ImageService.h"
#include "../Utils.h"
#include <wslutil.h>

namespace wslc::services
{

std::vector<ImageInformation> ImageService::List()
{
    auto session = OpenCLISession();
    wil::unique_cotaskmem_array_ptr<WSLA_IMAGE_INFORMATION> images;
    ULONG count = 0;
    THROW_IF_FAILED(session->ListImages(&images, &count));

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

void ImageService::Pull()
{
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

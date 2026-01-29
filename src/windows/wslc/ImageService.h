#pragma once

#include <JsonUtils.h>

namespace wslc::services
{
    struct ImageInformation
    {
        std::string Name;
        ULONGLONG Size;

        NLOHMANN_DEFINE_TYPE_INTRUSIVE_ONLY_SERIALIZE(ImageInformation, Name, Size);
    };

    class ImageService
    {
    public:
        std::vector<ImageInformation> List();
        void Pull(const std::string& image, IProgressCallback* callback);
        void Push();
        void Save();
        void Load();
        void Tag();
        void Prune();
        void Inspect();
    };
}

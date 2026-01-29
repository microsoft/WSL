#pragma once

namespace wslc::services
{
    struct ImageInformation
    {
        std::string Name;
        ULONGLONG Size;
    };

    class ImageService
    {
    public:
        std::vector<ImageInformation> List();
        void Pull();
        void Push();
        void Save();
        void Load();
        void Tag();
        void Prune();
        void Inspect();
    };
}

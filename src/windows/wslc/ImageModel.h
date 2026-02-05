#pragma once

#include <docker_schema.h>

namespace wslc::models {
    struct ImageInformation
    {
        std::string Name;
        ULONGLONG Size;

        NLOHMANN_DEFINE_TYPE_INTRUSIVE_ONLY_SERIALIZE(ImageInformation, Name, Size);
    };
}
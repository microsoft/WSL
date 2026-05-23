/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ExecutionContextData.h

Abstract:

    Header file for defining execution context data mappings.

--*/
#pragma once
#include "EnumVariantMap.h"
#include "ContainerModel.h"
#include "ImageModel.h"
#include "SessionModel.h"
#include "wslc.h"

#include <string>

#define DEFINE_DATA_MAPPING(_typeName_, _valueType_) \
    template <> \
    struct DataMapping<Data::_typeName_> \
    { \
        using value_t = _valueType_; \
    };

namespace wsl::windows::wslc::execution {
// Names a piece of data stored in the context by a task step.
// Must start at 0 to enable direct access to variant in Context.
// Max must be last and unused.
enum class Data : size_t
{
    Session,
    Containers,
    ContainerOptions,
    Images,
    Volumes,

    Max
};

namespace details {
    template <Data D>
    struct DataMapping
    {
    };

    DEFINE_DATA_MAPPING(Session, wsl::windows::wslc::models::Session);
    DEFINE_DATA_MAPPING(Containers, std::vector<wsl::windows::wslc::models::ContainerInformation>);
    DEFINE_DATA_MAPPING(ContainerOptions, wsl::windows::wslc::models::ContainerOptions);
    DEFINE_DATA_MAPPING(Images, std::vector<wsl::windows::wslc::models::ImageInformation>);
    DEFINE_DATA_MAPPING(Volumes, std::vector<WSLCVolumeInformation>);
} // namespace details

struct DataMap : wsl::windows::wslc::EnumBasedVariantMap<Data, wsl::windows::wslc::execution::details::DataMapping>
{
};
} // namespace wsl::windows::wslc::execution

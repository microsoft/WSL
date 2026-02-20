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
#include "SessionModel.h"

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

    Max
};

namespace details {
    template <Data D>
    struct DataMapping
    {
    };

    DEFINE_DATA_MAPPING(Session, wsl::windows::wslc::models::Session);
    DEFINE_DATA_MAPPING(Containers, std::vector<wsl::windows::wslc::models::ContainerInformation>);
} // namespace details

struct DataMap : wsl::windows::wslc::EnumBasedVariantMap<Data, wsl::windows::wslc::execution::details::DataMapping>
{
};
} // namespace wsl::windows::wslc::execution

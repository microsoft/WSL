// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#pragma once
#include "pch.h"

#include <string>

namespace wsl::windows::wslc::execution
{
    // Names a piece of data stored in the context by a task step.
    // Must start at 0 to enable direct access to variant in Context.
    // Max must be last and unused.
    enum class Data : size_t
    {
        SessionId,

        Max
    };

    namespace details
    {
        template <Data D>
        struct DataMapping
        {
            // value_t type specifies the type of this data
        };

        template<>
        struct DataMapping<Data::SessionId>
        {
            using value_t = std::wstring;
        };
    }
}
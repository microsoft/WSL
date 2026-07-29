// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include "ArgumentTypes.h"
#include "ContainerModel.h"
#include "InspectModel.h"

#include <cstdint>
#include <string>
#include <tuple>
#include <utility>
#include <wslc.h>

namespace wsl::windows::wslc::services {
struct BuildSecret;
}

namespace wsl::windows::wslc::argument::details {

// Local aliases so the ConvertedType tokens in the WSLC_ARGUMENTS X-macro (ArgumentDefinitions.h)
// resolve here regardless of include order. Aggregate converted types must be aliased because their
// commas would otherwise break X-macro argument parsing if written inline in the table.
using FormatType = wsl::windows::wslc::models::FormatType;
using InspectType = wsl::windows::wslc::models::InspectType;
using WSLCSignal = ::WSLCSignal;
using UlimitValue = std::tuple<std::string, int64_t, int64_t>;
using KeyValuePair = std::pair<std::string, std::string>;
using BuildSecret = wsl::windows::wslc::services::BuildSecret;

// Generate the ArgType -> converted type mapping from the X-macro. Every ArgType gets a
// specialization; arguments that are not converted map to NoConversion (their raw string is used
// directly at execution, and the validated cache accessors reject NoConversion at compile time).
#define WSLC_ARG_CONVERTED_MAPPING(EnumName, Name, Alias, Kind, ConvertedType, Desc) \
    template <> \
    struct ArgConvertedTypeMapping<ArgType::EnumName> \
    { \
        using value_t = ConvertedType; \
    };

WSLC_ARGUMENTS(WSLC_ARG_CONVERTED_MAPPING)
#undef WSLC_ARG_CONVERTED_MAPPING

} // namespace wsl::windows::wslc::argument::details

// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#pragma once
#include "pch.h"

// Macro to build localization function name and provide fallback
#define WSLC_LOC(_keyword_) \
    []() { \
        if constexpr (requires { wsl::shared::Localization::WSLCCLI_##_keyword_(); }) { \
            return wsl::shared::Localization::WSLCCLI_##_keyword_(); \
        } else { \
            return std::wstring{L#_keyword_}; \
        } \
    }()

// Macro to build localization function name and provide fallback
#define WSLC_LOC_ARG(_keyword_) \
    []() { \
        if constexpr (requires { wsl::shared::Localization::WSLCCLI_##_keyword_##ArgumentDescription(); }) { \
            return wsl::shared::Localization::WSLCCLI_##_keyword_##ArgumentDescription(); \
        } else { \
            return std::wstring{L#_keyword_}; \
        } \
    }()

// Macro to build localization function with an argument.
#define WSLC_LOC_1(_keyword_, _arg_) \
    []() { \
        if constexpr (requires { wsl::shared::Localization::WSLCCLI_##_keyword_##(_arg_); }) \
        { \
            return wsl::shared::Localization::WSLCCLI_##_keyword_##(_arg_ ); \
        } \
        else \
        { \
            return std::wstring{L#_keyword_ + L#_arg_}; \
        } \
    }()
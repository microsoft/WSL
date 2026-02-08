// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#pragma once

// The purpose of these macros is to create placeholder loc strings during development, before the
// localization strings are implemented. When the loc strings exist they will be replaced and used
// in the code, and when they do not exist, the macros will log the missing loc string ID at runtime.
// Intellisense will also indicate missing localization functions as errors, but these are not
// compile errors and can be ignored.

#ifdef __INTELLISENSE__
    // VS Intellisense doesn't handle this macro well and gives false positive errors when using it.
    // This will suppress some legitimate errors elsewhere, as unsuppressing does not work at end
    // of this file wherever the macro is used in other files. The errors that are not suppressed
    // will indicate when localization functions do not exist to identify missing loc string IDs.
    // This only suppresses Intellisense and does not affect the compiler finding legitimate errors.
    #pragma diag_suppress 299, 439, 40, 2841, 28
#endif

// Macro to build localization function name for argument descriptions with fallback
#define WSLC_LOC_ARG(_keyword_) \
    []() { \
        if constexpr (requires { wsl::shared::Localization::WSLCCLI_##_keyword_##ArgumentDescription(); }) { \
            return wsl::shared::Localization::WSLCCLI_##_keyword_##ArgumentDescription(); \
        } else { \
            WSLC_LOG(Debug, Verbose, << L"MISSING LOCALIZATION: WSLCCLI_" << L ## #_keyword_ << L"ArgumentDescription"); \
            return std::wstring{L ## #_keyword_}; \
        } \
    }()

// Variadic macro to build localization function with any number of arguments
// Uses two-phase approach: compile-time detection via concept, runtime invocation.
#define WSLC_LOC(_keyword_, ...) \
    [&]() -> std::wstring { \
        using _Loc_NS = wsl::shared::Localization; \
        if constexpr (requires { _Loc_NS::WSLCCLI_##_keyword_; }) \
        { \
            if constexpr (std::is_invocable_v<decltype(&_Loc_NS::WSLCCLI_##_keyword_), decltype(__VA_ARGS__)...>) \
            { \
                return _Loc_NS::WSLCCLI_##_keyword_(__VA_ARGS__); \
            } \
            else \
            { \
                WSLC_LOG(Debug, Verbose, << L"MISSING LOCALIZATION: WSLCCLI_" << L## #_keyword_ << L" (wrong signature)"); \
                return std::wstring{L## #_keyword_}; \
            } \
        } \
        else \
        { \
            WSLC_LOG(Debug, Verbose, << L"MISSING LOCALIZATION: WSLCCLI_" << L## #_keyword_); \
            return std::wstring{L## #_keyword_}; \
        } \
    }()

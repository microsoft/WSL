// Copyright (C) Microsoft Corporation. All rights reserved.
//
// Defines macros similar to those in <wil/result_macros.h> for use with kernel mode components.
// Also defines result macros for use with <unexpected.h> for either kernel or user mode.
#pragma once

/// Static information for an error site.
struct ErrorSite
{
    PCSTR file;
    PCSTR function;
    int line;
    PCSTR messageFormat;
};

// No logging for Linux at the moment.
#define ON_FAILURE_WITH_SOURCE(str, status)
#define ON_FAILURE_WITH_SOURCE_MSG(str, status, messageFormat, ...)

#define RETURN_ERROR_IF_UNEXPECTED(expected) \
    do \
    { \
        auto __localError = (expected).OptionalError(); \
        if (__localError) \
        { \
            ON_FAILURE_WITH_SOURCE(#expected, *__localError); \
            return *__localError; \
        } \
    } while (0)

#define RETURN_ERROR_IF_UNEXPECTED_MSG(expected, messageFormat, ...) \
    do \
    { \
        auto __localError = (expected).OptionalError(); \
        if (__localError) \
        { \
            ON_FAILURE_WITH_SOURCE_MSG(#expected, *__localError, messageFormat, __VA_ARGS__); \
            return *__localError; \
        } \
    } while (0)

#define RETURN_IF_UNEXPECTED(expected) \
    do \
    { \
        auto __localError = (expected).OptionalError(); \
        if (__localError) \
        { \
            ON_FAILURE_WITH_SOURCE(#expected, *__localError); \
            return ::util::Unexpected{*__localError}; \
        } \
    } while (0)

#define RETURN_IF_UNEXPECTED_MSG(expected, messageFormat, ...) \
    do \
    { \
        auto __localError = (expected).OptionalError(); \
        if (__localError) \
        { \
            ON_FAILURE_WITH_SOURCE_MSG(#expected, *__localError, messageFormat, __VA_ARGS__); \
            return ::util::Unexpected{*__localError}; \
        } \
    } while (0)

#define RETURN_UNEXPECTED_IF_NTSTATUS_FAILED(status) \
    do \
    { \
        NTSTATUS __localStatus = (status); \
        if (!NT_SUCCESS(__localStatus)) \
        { \
            ON_FAILURE_WITH_SOURCE(#status, __localStatus); \
            return ::util::Unexpected{__localStatus}; \
        } \
    } while (0)

#define RETURN_UNEXPECTED_IF_NTSTATUS_FAILED_MSG(status, message, ...) \
    do \
    { \
        NTSTATUS __localStatus = (status); \
        if (!NT_SUCCESS(__localStatus)) \
        { \
            ON_FAILURE_WITH_SOURCE_MSG(#status, __localStatus, message, __VA_ARGS__); \
            return ::util::Unexpected{__localStatus}; \
        } \
    } while (0)

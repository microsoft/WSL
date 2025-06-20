// Copyright (C) Microsoft Corporation. All rights reserved.
#include <stdexcept>
#include <optional>
#include <source_location>

#pragma once

class RuntimeErrorWithSourceLocation : public std::runtime_error
{
public:
    RuntimeErrorWithSourceLocation(const std::string& reason, const std::source_location& location = std::source_location::current());

    RuntimeErrorWithSourceLocation(
        const std::string& reason, const std::exception& inner, const std::source_location& location = std::source_location::current());

private:
    static std::string BuildMessage(const std::string& reason, const std::optional<std::exception>& inner, const std::source_location& location);
};

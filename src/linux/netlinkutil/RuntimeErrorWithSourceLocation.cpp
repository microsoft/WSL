// Copyright (C) Microsoft Corporation. All rights reserved.
#include <sstream>
#include <optional>
#include "RuntimeErrorWithSourceLocation.h"

RuntimeErrorWithSourceLocation::RuntimeErrorWithSourceLocation(const std::string& reason, const std::source_location& location) :
    std::runtime_error(BuildMessage(reason, {}, location))
{
}

RuntimeErrorWithSourceLocation::RuntimeErrorWithSourceLocation(const std::string& reason, const std::exception& inner, const std::source_location& location) :
    std::runtime_error(BuildMessage(reason, inner, location))
{
}

std::string RuntimeErrorWithSourceLocation::BuildMessage(const std::string& reason, const std::optional<std::exception>& inner, const std::source_location& source)
{
    std::stringstream str;
    str << "Exception thrown by " << source.function_name() << " in " << source.file_name() << ":" << source.line() << ":" << reason;
    if (inner.has_value())
    {
        str << "\nInner exception: " << inner.value().what();
    }

    return str.str();
}

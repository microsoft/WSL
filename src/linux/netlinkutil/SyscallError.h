// Copyright (C) Microsoft Corporation. All rights reserved.
#pragma once

#include "RuntimeErrorWithSourceLocation.h"

class SyscallError : public RuntimeErrorWithSourceLocation
{
public:
    SyscallError(const std::string& method, const std::string& arguments, int err, const std::source_location& source);

    static std::string BuildMessage(const std::string& method, const std::string& arguments, int err);

    int GetErrno() const;

private:
    int savedErrno;
};

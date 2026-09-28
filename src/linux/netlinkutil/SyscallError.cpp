// Copyright (C) Microsoft Corporation. All rights reserved.
#include <sstream>
#include <string.h>
#include "SyscallError.h"

SyscallError::SyscallError(const std::string& method, const std::string& arguments, int err, const std::source_location& source) :
    RuntimeErrorWithSourceLocation(BuildMessage(method, arguments, err), source), savedErrno(err)
{
}

std::string SyscallError::BuildMessage(const std::string& method, const std::string& arguments, int err)
{
    std::stringstream str;

    str << method << "(" << arguments << ") failed with errno=" << err << " (" << strerror(err) << ")";

    return str.str();
}

int SyscallError::GetErrno() const
{
    return savedErrno;
}

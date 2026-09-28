// Copyright (C) Microsoft Corporation. All rights reserved.
#pragma once

#include "p9defs.h"
#include "expected.h"
#include "result_macros.h"

namespace p9fs {

using util::BasicExpected;
using util::Unexpected;

using LxError = Unexpected<LX_INT>;

template <typename T>
using Expected = BasicExpected<T, LX_INT>;

namespace util {

    LX_INT LinuxErrorFromCaughtException();

} // namespace util

} // namespace p9fs

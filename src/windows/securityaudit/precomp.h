// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include <winsock2.h>
#include <windows.h>
#include <winternl.h>
// ntsecapi otherwise redeclares the NT string types already supplied by winternl.
#ifndef _NTDEF_
#define _NTDEF_
#include <ntsecapi.h>
#undef _NTDEF_
#else
#include <ntsecapi.h>
#endif
#include <sddl.h>
#include <wil/resource.h>
#include <wil/result.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "Protocol.h"

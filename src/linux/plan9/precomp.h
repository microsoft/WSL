// Copyright (C) Microsoft Corporation. All rights reserved.
#pragma once

#include <sys/time.h>
#include <sys/epoll.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/xattr.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <aio.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/uio.h>

// C standard library
#include <cstdint>
#include <cstdio>
#include <cwctype>

// C++ standard library
#include <exception>
#include <vector>
#include <queue>
#include <list>
#include <map>
#include <variant>
#include <optional>
#include <chrono>
#include <thread>
#include <mutex>
#include <shared_mutex>
#include <atomic>
#include <string>
#include <string_view>
#include <filesystem>

// Guideline Support Library
#include <gsl/gsl>
#include <gsl/algorithm>

#include <coroutine>
#include <cassert>
#include <dirent.h>
#include "lxdef.h"
#include "lxwil.h"

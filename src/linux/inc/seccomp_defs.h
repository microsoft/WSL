// Copyright (C) Microsoft Corporation. All rights reserved.

// This file defines all the seccomp structures and constants that aren't available in the sdk yet

#pragma once

#include <unistd.h>

// From include\asm\unist_32.h
// Read socketcall.2, there is no socketcall systemcall on x86_64 systems.
// You will not get this value with the standard defines.
#define I386_NR_socketcall (102)
#define ARMV7_NR_bind (282)

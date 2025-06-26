#pragma once

#include "wslservice.h"

HRESULT GetWslVersion(WSL_VERSION* Version);

HRESULT CreateVm(const VIRTUAL_MACHINE_SETTINGS* Settings, ILSWVirtualMachine** VirtualMachine);
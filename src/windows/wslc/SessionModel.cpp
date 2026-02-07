#include "SessionModel.h"

namespace wslc::models {
SessionOptions SessionOptions::Default()
{
    // TODO: Have a configuration file for those.
    auto dataFolder = std::filesystem::path(wsl::windows::common::filesystem::GetLocalAppDataPath(nullptr)) / "wsla";
    SessionOptions options{};
    options.DisplayName = L"wsla-cli";
    options.CpuCount = 4;
    options.MemoryMb = 2048;
    options.BootTimeoutMs = 30 * 1000;
    options.StoragePath = dataFolder.c_str();
    options.MaximumStorageSizeMb = 10000; // 10GB.
    options.NetworkingMode = WSLANetworkingModeNAT;
    return options;
}
}
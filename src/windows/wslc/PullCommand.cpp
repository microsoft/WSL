#include "precomp.h"
#include "PullCommand.h"
#include "Utils.h"
#include "WSLAProcessLauncher.h"
#include <CommandLine.h>

using namespace wsl::shared;
namespace wslutil = wsl::windows::common::wslutil;
using wsl::windows::common::ClientRunningWSLAProcess;
using wsl::windows::common::Context;
using wsl::windows::common::ExecutionContext;
using wsl::windows::common::WSLAProcessLauncher;
using wsl::windows::common::relay::EventHandle;
using wsl::windows::common::relay::MultiHandleWait;
using wsl::windows::common::relay::RelayHandle;
using wsl::windows::common::wslutil::WSLAErrorDetails;

int RunPullCommand(std::wstring_view commandLine)
{
    ArgumentParser parser(std::wstring{commandLine}, L"wslc", 2);

    std::string image;
    parser.AddPositionalArgument(Utf8String{image}, 0);

    parser.Parse();
    THROW_HR_IF(E_INVALIDARG, image.empty());

    PullImpl(*OpenCLISession(), image);

    return 0;
}
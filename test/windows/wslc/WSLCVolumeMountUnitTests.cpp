/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCCLIParserUnitTests.cpp

Abstract:

    This file contains unit tests for WSLC CLI argument parsing and validation.

--*/

#include "precomp.h"
#include "windows/Common.h"
#include "ContainerModel.h"

using namespace wsl::windows::wslc::models;

namespace WSLCVolumeMount {

class WSLCVolumeMountUnitTests
{
    WSL_TEST_CLASS(WSLCVolumeMountUnitTests)

    TEST_METHOD(VolumeMount_Parse_ReturnExpectedResult)
    {
        // Volume value => host, contianer, reaodnly
        std::vector<std::tuple<std::wstring, std::wstring, std::string, bool>> validVolumeArgs = {
            {LR"(C:\hostPath:/containerPath)", LR"(C:\hostPath)", R"(/containerPath)", false},
            {LR"(C:\hostPath:/containerPath:ro)", LR"(C:\hostPath)", R"(/containerPath)", true},
            {LR"(C:\hostPath:/containerPath:rw)", LR"(C:\hostPath)", R"(/containerPath)", false},
            {LR"(C:\host Path:/container Path:ro)", LR"(C:\host Path)", R"(/container Path)", true},
            {LR"(C:\host Path:/container Path:rw)", LR"(C:\host Path)", R"(/container Path)", false},
            {LR"(:/containerPath)", LR"()", R"(/containerPath)", false},
            {LR"(C:\hostPath::ro)", LR"(C:\hostPath)", R"()", true},
            {LR"(:/containerPath:ro)", LR"()", R"(/containerPath)", true},
            {LR"(C:\hostPath:)", LR"(C:\hostPath)", R"()", false},
            {LR"(C:\hostPath:ro)", LR"(C)", R"(\hostPath)", true},
            {LR"(C:\hostPath::rw)", LR"(C:\hostPath)", R"()", false},
            {LR"(C:\hostPath:/containerPath:invalid_mode)", LR"(C:\hostPath:/containerPath)", R"(invalid_mode)", false},
            {LR"(C:\hostPath:/containerPath:ro:extra)", LR"(C:\hostPath:/containerPath:ro)", R"(extra)", false},
            {LR"(C:\hostPath:/containerPath:)", LR"(C:\hostPath:/containerPath)", R"()", false},
            {LR"(C:\hostPath)", LR"(C)", R"(\hostPath)", false},
            {LR"(C:/hostPath)", LR"(C)", R"(/hostPath)", false},
            {LR"(:)", LR"()", R"()", false},
            {LR"(::)", LR"(:)", R"()", false},
        };

        for (const auto& arg : validVolumeArgs)
        {
            WEX::Logging::Log::Comment(std::format(L"Testing volume argument: '{}'", std::get<0>(arg)).c_str());
            auto result = VolumeMount::Parse(std::get<0>(arg));
            VERIFY_ARE_EQUAL(result.HostPath(), std::get<1>(arg));
            VERIFY_ARE_EQUAL(result.ContainerPath(), std::get<2>(arg));
            VERIFY_ARE_EQUAL(result.IsReadOnly(), std::get<3>(arg));
        }
    }
};
} // namespace WSLCVolumeMount
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

    TEST_METHOD(VolumeMount_Parse_Success)
    {
        // Volume value => host, contianer, reaodnly
        std::vector<std::tuple<std::wstring, std::wstring, std::string, bool>> validVolumeArgs = {
            { LR"(C:\hostPath:/containerPath)", LR"(C:\hostPath)", R"(/containerPath)", false },
            { LR"(C:\hostPath:/containerPath:ro)", LR"(C:\hostPath)", R"(/containerPath)", true },
            { LR"(C:\hostPath:/containerPath:rw)", LR"(C:\hostPath)", R"(/containerPath)", false },
            { LR"(C:\host Path:/container Path:ro)", LR"(C:\host Path)", R"(/container Path)", true },
            { LR"(C:\host Path:/container Path:rw)", LR"(C:\host Path)", R"(/container Path)", false },
        };
        
        for (const auto& arg : validVolumeArgs)
        {
            auto result = VolumeMount::Parse(std::get<0>(arg));
            VERIFY_ARE_EQUAL(result.HostPath(), std::get<1>(arg));
            VERIFY_ARE_EQUAL(result.ContainerPath(), std::get<2>(arg));
            VERIFY_ARE_EQUAL(result.IsReadOnly(), std::get<3>(arg));
        }
    }

    TEST_METHOD(VolumeMount_Parse_Failure)
    {
        std::vector<std::wstring> invalidVolumeArgs = {
            LR"(C:\hostPath)", // Missing container path
            LR"(:/containerPath)", // Missing host path
            LR"(C:\hostPath:/containerPath:invalid_mode)", // Invalid mode
            LR"(C:\hostPath:/containerPath:ro:extra)", // Too many colons
            LR"(C:\hostPath::ro)", // Missing container path with mode
            LR"(:/containerPath:ro)", // Missing host path with mode
            LR"(C:\hostPath:/containerPath:)", // Empty mode
            LR"()", // Empty string
            LR"(:)", // Empty host and container
            LR"(::)", // Empty host and container with extra delimiter
            LR"(C:\hostPath:)", // Empty container
            LR"(C:\hostPath:ro)", // Mode provided but missing container path
            LR"(:ro)", // Empty host/container with mode token
            LR"(C:\hostPath::rw)", // Empty container with valid mode
            LR"(C:\hostPath:/containerPath:readonly:)", // Trailing delimiter
        };

        for (const auto& arg : invalidVolumeArgs)
        {
            VERIFY_THROWS(VolumeMount::Parse(arg), std::exception);
        }
    }
};
} // namespace WSLCCLIParserUnitTests
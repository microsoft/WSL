/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    Session.h

Abstract:

    This file contains the definition of the WinRT wrapper for the WSLC SDK Session class.

--*/

#pragma once
#include "Microsoft.WSL.Containers.Session.g.h"
#include "SessionSettings.h"
#include "Helpers.h"

namespace winrt::Microsoft::WSL::Containers::implementation {
struct Session : SessionT<Session>
{
    Session() = default;
    ~Session();

    static winrt::Microsoft::WSL::Containers::Session Create(winrt::Microsoft::WSL::Containers::SessionSettings const& settings);
    void Terminate();

    WslcSession ToHandle();
    WslcSession* ToHandlePointer();

private:
    WslcSession m_session{nullptr};
};
} // namespace winrt::Microsoft::WSL::Containers::implementation

namespace winrt::Microsoft::WSL::Containers::factory_implementation {
struct Session : SessionT<Session, implementation::Session>
{
};
} // namespace winrt::Microsoft::WSL::Containers::factory_implementation

DEFINE_TYPE_HELPERS(Session);
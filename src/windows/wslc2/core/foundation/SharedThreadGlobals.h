// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#pragma once
#include "pch.h"
#include "Logging.h"

namespace wsl::windows::wslc::logging::threadlocalstorage
{
    struct PreviousThreadGlobals;

    // Interface for access to values that are stored on a per-thread object.
    struct ThreadGlobals
    {
        ThreadGlobals() = default;
        virtual ~ThreadGlobals() = default;

        virtual wsl::windows::wslc::logging::DiagnosticLogger& GetDiagnosticLogger() = 0;

        virtual void* GetTelemetryObject() = 0;

        // Set Globals for Current Thread
        // Return RAII object with its ownership to set the AppInstaller ThreadLocalStorage back to previous state
        virtual std::unique_ptr<PreviousThreadGlobals> SetForCurrentThread();

        // Return Globals for Current Thread
        static ThreadGlobals* GetForCurrentThread();
    };

    // RAII object used to enable reverting back to the previous thread globals object.
    struct PreviousThreadGlobals
    {
        ~PreviousThreadGlobals();

        PreviousThreadGlobals(ThreadGlobals* previous);

    private:
        ThreadGlobals* m_previous;
        DWORD m_threadId;
    };
}
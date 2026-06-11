// Copyright (C) Microsoft Corporation. All rights reserved.

//
// Adapters that bridge the SDK-facing WSLCSDK callback interfaces (declared in
// WSLSDK.idl) to the internal wslc.idl callback interfaces. The service classes
// implement the IWSLCSDK* interfaces by converting the SDK callback objects into
// these adapters and forwarding to the existing wslc.idl based implementations.
//
// The adapters are only ever invoked in-process (they wrap the SDK's marshaled
// proxy), so they do not need to be registered for marshaling.
//

#pragma once

#include "wslc.h"
#include "WSLSDK.h"
#include <wrl/implements.h>

namespace wsl::windows::common::wslcsdk {

class TerminationCallbackAdapter
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, ITerminationCallback>
{
public:
    TerminationCallbackAdapter(IWSLCSDKTerminationCallback* Inner) : m_inner(Inner)
    {
    }

    IFACEMETHOD(OnTermination)(WSLCVirtualMachineTerminationReason Reason, LPCWSTR Details) override
    {
        return m_inner->OnTermination(static_cast<WSLCSDKVirtualMachineTerminationReason>(Reason), Details);
    }

private:
    Microsoft::WRL::ComPtr<IWSLCSDKTerminationCallback> m_inner;
};

class CrashDumpCallbackAdapter
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, ICrashDumpCallback>
{
public:
    CrashDumpCallbackAdapter(IWSLCSDKCrashDumpCallback* Inner) : m_inner(Inner)
    {
    }

    IFACEMETHOD(OnCrashDump)(LPCWSTR DumpPath, LPCSTR ProcessName, ULONGLONG Pid, ULONG Signal, ULONGLONG Timestamp) override
    {
        return m_inner->OnCrashDump(DumpPath, ProcessName, Pid, Signal, Timestamp);
    }

private:
    Microsoft::WRL::ComPtr<IWSLCSDKCrashDumpCallback> m_inner;
};

class ProgressCallbackAdapter
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IProgressCallback>
{
public:
    ProgressCallbackAdapter(IWSLCSDKProgressCallback* Inner) : m_inner(Inner)
    {
    }

    IFACEMETHOD(OnProgress)(LPCSTR Status, LPCSTR Id, ULONGLONG Current, ULONGLONG Total) override
    {
        return m_inner->OnProgress(Status, Id, Current, Total);
    }

private:
    Microsoft::WRL::ComPtr<IWSLCSDKProgressCallback> m_inner;
};

class WarningCallbackAdapter
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IWarningCallback>
{
public:
    WarningCallbackAdapter(IWSLCSDKWarningCallback* Inner) : m_inner(Inner)
    {
    }

    IFACEMETHOD(OnWarning)(LPCWSTR Message) override
    {
        return m_inner->OnWarning(Message);
    }

private:
    Microsoft::WRL::ComPtr<IWSLCSDKWarningCallback> m_inner;
};

// Helpers that return a wslc.idl callback wrapping the given SDK callback, or
// nullptr when the SDK callback is null.

inline Microsoft::WRL::ComPtr<ITerminationCallback> WrapTerminationCallback(IWSLCSDKTerminationCallback* Callback)
{
    Microsoft::WRL::ComPtr<ITerminationCallback> result;
    if (Callback != nullptr)
    {
        result = Microsoft::WRL::Make<TerminationCallbackAdapter>(Callback);
    }

    return result;
}

inline Microsoft::WRL::ComPtr<ICrashDumpCallback> WrapCrashDumpCallback(IWSLCSDKCrashDumpCallback* Callback)
{
    Microsoft::WRL::ComPtr<ICrashDumpCallback> result;
    if (Callback != nullptr)
    {
        result = Microsoft::WRL::Make<CrashDumpCallbackAdapter>(Callback);
    }

    return result;
}

inline Microsoft::WRL::ComPtr<IProgressCallback> WrapProgressCallback(IWSLCSDKProgressCallback* Callback)
{
    Microsoft::WRL::ComPtr<IProgressCallback> result;
    if (Callback != nullptr)
    {
        result = Microsoft::WRL::Make<ProgressCallbackAdapter>(Callback);
    }

    return result;
}

inline Microsoft::WRL::ComPtr<IWarningCallback> WrapWarningCallback(IWSLCSDKWarningCallback* Callback)
{
    Microsoft::WRL::ComPtr<IWarningCallback> result;
    if (Callback != nullptr)
    {
        result = Microsoft::WRL::Make<WarningCallbackAdapter>(Callback);
    }

    return result;
}

} // namespace wsl::windows::common::wslcsdk

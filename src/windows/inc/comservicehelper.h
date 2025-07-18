///////////////////////////////////////////////////////////////////////////////
//                                                                           //
// Copyright (c) Microsoft Corporation. All rights reserved.                 //
// comservicehelper.h                                                        //
//                                                                           //
// Provides a template class to handle a Service entry point.                //
//                                                                           //
///////////////////////////////////////////////////////////////////////////////

#pragma once

#include <sddl.h>
#include <ctxtcall.h>
#include <wil\result.h>
#include <wil\resource.h>

namespace Windows { namespace Internal {

    struct ServerDescriptor final
    {
        const wchar_t* ServerName = nullptr;
    };

    template <typename TBase>
    struct ModuleServerDescriptor
    {
        constexpr static const ServerDescriptor Create()
        {
            constexpr const ServerDescriptor serverDescriptor = {TBase::ServerName};
            return serverDescriptor;
        }
    };

    struct DefaultServerDescriptor final
    {
    };

    class ServiceModuleBase
    {
    public:
        ServiceModuleBase()
        {
        }

        ~ServiceModuleBase()
        {
        }

        template <typename TSecurityPolicy, GLOBALOPT_EH_VALUES TExceptionPolicy, typename TServerDescriptor = DefaultServerDescriptor>
        HRESULT Initialize(_In_ boolean ownProcess, _In_ boolean addRefModule, _In_ boolean hasDedicatedThread = true, _In_ HANDLE stopEvent = nullptr)
        {
            auto uninitializeOnFailure = wil::scope_exit([&]() { Uninitialize(); });

            if (hasDedicatedThread)
            {
                // If the ServiceModule is being initialized on its own dedicated thread (i.e. the thread hangs around until it's
                // time to call SvcModuleBase::Uninitialize) then initialize COM for this thread.
                m_hrMtaInitialized = Windows::Foundation::Initialize(RO_INIT_MULTITHREADED);
            }
            else
            {
                // Otherwise, take a reference on the MTA apartment.
                m_hrMtaInitialized = CoIncrementMTAUsage(&m_mtaUsageCookie);
            }
            RETURN_IF_FAILED(m_hrMtaInitialized);

            __if_exists(TServerDescriptor::Create)
            {
                m_serverDescriptor = TServerDescriptor::Create();
            }

            if (ownProcess)
            {
                RETURN_IF_FAILED(InitializeSecurity<TSecurityPolicy>());
            }

            // Tell COM how to mask fatal exceptions.
            if (ownProcess)
            {
                Microsoft::WRL::ComPtr<IGlobalOptions> pIGLB;
                RETURN_IF_FAILED(CoCreateInstance(CLSID_GlobalOptions, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pIGLB)));
                RETURN_IF_FAILED(pIGLB->Set(COMGLB_EXCEPTION_HANDLING, TExceptionPolicy));
            }

            // SubInitialize must be called before IncrementObjectCount or the ContextCallback.
            // The ContextCallback will register the COM objects, and as soon as that happens, incoming activations may arrive
            // which will call IncrementObjectCount.
            RETURN_IF_FAILED(SubInitialize());

            // Add the extra module reference to prevent shutdown before the ContextCallback because once we register the COM objects,
            // an object may be released and drop the module reference count to zero if the extra reference isn't added yet.
            if (addRefModule)
            {
                IncrementObjectCount();
                m_addedModuleReference = true;
            }

            RETURN_IF_FAILED(CoCreateInstance(CLSID_ContextSwitcher, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&m_icc)));

            RETURN_IF_FAILED(m_icc->ContextCallback(
                &Windows::Internal::ServiceModuleBase::ConnectCallbackThunk, reinterpret_cast<ComCallData*>(this), IID_IContextCallback, 5, nullptr));

            uninitializeOnFailure.release();
            return S_OK;
        }

        HRESULT Uninitialize()
        {
            if (m_icc)
            {
                m_icc->ContextCallback(
                    &ServiceModuleBase::DisconnectCallbackThunk, reinterpret_cast<ComCallData*>(this), IID_IContextCallback, 5, nullptr);
                m_icc = nullptr;
            }

            if (m_addedModuleReference)
            {
                DecrementObjectCount();
                m_addedModuleReference = false;
            }

            if (SUCCEEDED(m_hrMtaInitialized))
            {
                if (m_mtaUsageCookie)
                {
                    m_mtaUsageCookie.reset();
                }
                else
                {
                    Windows::Foundation::Uninitialize();
                }

                m_hrMtaInitialized = E_FAIL;
            }
            return S_OK;
        }

        virtual HRESULT ConnectCallback() = 0;

        virtual HRESULT DisconnectCallback() = 0;

        STDMETHOD_(ULONG, IncrementObjectCount()) = 0;

        STDMETHOD_(ULONG, DecrementObjectCount()) = 0;

        static HRESULT __stdcall ConnectCallbackThunk(_In_ ComCallData* pv)
        {
            ServiceModuleBase* pThis = reinterpret_cast<ServiceModuleBase*>(pv);
            return pThis->ConnectCallback();
        }

        static HRESULT __stdcall DisconnectCallbackThunk(_In_ ComCallData* pv)
        {
            ServiceModuleBase* pThis = reinterpret_cast<ServiceModuleBase*>(pv);
            return pThis->DisconnectCallback();
        }

    public:
        //
        // These are not fully-fledged policy objects, but they all rely on SDDL instead.
        //
        // Useful references:
        //
        // Access Control Lists for COM
        //   http://msdn.microsoft.com/en-us/library/windows/desktop/ms693364(v=vs.85).aspx
        //
        // Security Descriptor String Format
        //   http://msdn.microsoft.com/en-us/library/windows/desktop/aa379570(v=vs.85).aspx
        //
        // ACE Strings
        //   http://msdn.microsoft.com/en-us/library/windows/desktop/aa374928(v=vs.85).aspx
        //
        struct SecurityPolicyEveryoneLocal
        {
            static LPCWSTR GetSDDLText()
            {
                //
                // The current one explicitly allows Everyone and App Packages for local clients only.
                //
                // O: = Owner
                // PS = principal self
                // G: = Group
                // BU = Built-in users
                // D: = DACL
                // A  = access allowed
                // 0B = COM_RIGHTS_EXECUTE | COM_RIGHTS_EXECUTE_LOCAL | COM_RIGHTS_ACTIVATE_LOCAL
                // AC = App Packages
                // WD = everyone
                // S: = SACL
                // ML = Mandatory Label
                // NX = NO_EXECUTE_UP
                // LW = Low Integrity
                //
                return L"O:PSG:BUD:(A;;0xB;;;AC)(A;;0xB;;;WD)S:(ML;;NX;;;LW)";
            }
        };

        struct SecurityPolicyEveryoneLocalAndRemote
        {
            static LPCWSTR GetSDDLText()
            {
                //
                // The current one explicitly allows Everyone and App Packages for local and remote clients.
                //
                // O: = Owner
                // PS = principal self
                // G: = Group
                // BU = Built-in users
                // D: = DACL
                // A  = access allowed
                // 1F = COM_RIGHTS_EXECUTE | COM_RIGHTS_EXECUTE_LOCAL | COM_RIGHTS_ACTIVATE_LOCAL | COM_RIGHTS_EXECUTE_REMOTE | COM_RIGHTS_ACTIVATE_REMOTE
                // AC = App Packages
                // WD = everyone
                // S: = SACL
                // ML = Mandatory Label
                // NX = NO_EXECUTE_UP
                // LW = Low Integrity
                //
                return L"O:PSG:BUD:(A;;0x1F;;;AC)(A;;0x1F;;;WD)S:(ML;;NX;;;LW)";
            }
        };

    protected:
        // _module is a reference, so the compiler can't generate these. Hide them.
        ServiceModuleBase(const ServiceModuleBase&);
        ServiceModuleBase& operator=(const ServiceModuleBase&);

        // Used by derived classes to initialize any necessary state
        virtual HRESULT SubInitialize()
        {
            return S_OK;
        }

        template <typename TSecurityPolicy>
        HRESULT InitializeSecurity()
        {
            PACL pDacl = nullptr, pSacl = nullptr;
            PSID pOwner = nullptr, pPrimaryGroup = nullptr;
            PSECURITY_DESCRIPTOR pSDRelative = nullptr, pSDAbsolute = nullptr;
            DWORD cbSDAbsolute = 0, cbDacl = 0, cbSacl = 0, cbOwner = 0, cbPrimaryGroup = 0;

            auto cleanup = wil::scope_exit([&] {
                HeapFree(GetProcessHeap(), 0, pDacl);
                HeapFree(GetProcessHeap(), 0, pSacl);
                HeapFree(GetProcessHeap(), 0, pOwner);
                HeapFree(GetProcessHeap(), 0, pPrimaryGroup);
                HeapFree(GetProcessHeap(), 0, pSDAbsolute);
                HeapFree(GetProcessHeap(), 0, pSDRelative);
            });

            // The following call returns a self-relative security descriptor...
            RETURN_IF_WIN32_BOOL_FALSE(ConvertStringSecurityDescriptorToSecurityDescriptor(
                TSecurityPolicy::GetSDDLText(), SDDL_REVISION_1, &pSDRelative, nullptr));

            // ...before we pass it to CoInitializeSecurity, we need to make it absolute. We call MakeAbsoluteSD once to find out how large our buffers need to be...
            RETURN_LAST_ERROR_IF(
                MakeAbsoluteSD(pSDRelative, nullptr, &cbSDAbsolute, nullptr, &cbDacl, nullptr, &cbSacl, nullptr, &cbOwner, nullptr, &cbPrimaryGroup) ||
                ERROR_INSUFFICIENT_BUFFER != GetLastError());

            // Then we allocate the buffers...
            pSDAbsolute = reinterpret_cast<PSECURITY_DESCRIPTOR>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, cbSDAbsolute));
            RETURN_IF_NULL_ALLOC(pSDAbsolute);

            pDacl = reinterpret_cast<PACL>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, cbDacl));
            RETURN_IF_NULL_ALLOC(pDacl);

            pSacl = reinterpret_cast<PACL>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, cbSacl));
            RETURN_IF_NULL_ALLOC(pSacl);

            pOwner = reinterpret_cast<PSID>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, cbOwner));
            RETURN_IF_NULL_ALLOC(pOwner);

            pPrimaryGroup = reinterpret_cast<PSID>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, cbPrimaryGroup));
            RETURN_IF_NULL_ALLOC(pPrimaryGroup);

            // ...then we call MakeAbsoluteSD again with the buffers we just allocated
            RETURN_IF_WIN32_BOOL_FALSE(MakeAbsoluteSD(
                pSDRelative, pSDAbsolute, &cbSDAbsolute, pDacl, &cbDacl, pSacl, &cbSacl, pOwner, &cbOwner, pPrimaryGroup, &cbPrimaryGroup));

            // ...and now we can call CoInitializeSecurity
            RETURN_IF_FAILED(CoInitializeSecurity(
                pSDAbsolute, -1, nullptr, nullptr, RPC_C_AUTHN_LEVEL_DEFAULT, RPC_C_IMP_LEVEL_IDENTIFY, NULL, EOAC_NONE, nullptr));

            return S_OK;
        }

        // Declare before any COM member variables in this object
        wil::unique_mta_usage_cookie m_mtaUsageCookie;

        // Result of initializing the COM apartment
        HRESULT m_hrMtaInitialized = E_FAIL;

        // Track whether we added an extra module reference
        bool m_addedModuleReference = false;

        // COM callback object to support unloading shared-process services
        Microsoft::WRL::ComPtr<IContextCallback> m_icc;

        // COM Server descriptor
        ServerDescriptor m_serverDescriptor{};
    };

    class ServiceModule : public ServiceModuleBase, public Microsoft::WRL::Module<Microsoft::WRL::OutOfProc, ServiceModule>
    {
    public:
        STDMETHOD_(ULONG, IncrementObjectCount()) override
        {
            return Microsoft::WRL::Module<Microsoft::WRL::OutOfProc, ServiceModule>::IncrementObjectCount();
        }

        STDMETHOD_(ULONG, DecrementObjectCount()) override
        {
            return Microsoft::WRL::Module<Microsoft::WRL::OutOfProc, ServiceModule>::DecrementObjectCount();
        }

        HRESULT ConnectCallback() override
        {
            return __super::RegisterObjects(m_serverDescriptor.ServerName);
        }

        HRESULT DisconnectCallback() override
        {
            __super::UnregisterObjects(m_serverDescriptor.ServerName);
            return CoDisconnectContext(INFINITE);
        }
    };

    enum LastObjectReleaseBehavior
    {
        ShutdownAfterLastObjectReleased = 1,
        ContinueRunningWithNoObjects = 2,
    };

    template <
        typename TBase,
        LastObjectReleaseBehavior TLastObjectReleaseBehavior = ShutdownAfterLastObjectReleased,
        typename TSecurityPolicy = ServiceModule::SecurityPolicyEveryoneLocal,
        GLOBALOPT_EH_VALUES TExceptionPolicy = COMGLB_EXCEPTION_DONOT_HANDLE_ANY,
        typename TServerDescriptor = DefaultServerDescriptor>
    class Service
    {
    public:
        Service()
        {
            _serviceStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
            _serviceStatus.dwCurrentState = SERVICE_RUNNING;
            _serviceStatus.dwWin32ExitCode = NO_ERROR;
        }

        ~Service()
        {
            __if_exists(TBase::OnLowPowerModeChanged)
            {
                if (_powerHandle != nullptr)
                {
                    PowerSettingUnregisterNotification(_powerHandle);
                    _powerHandle = nullptr;
                }
            }

            if (_stopEvent != nullptr)
            {
                CloseHandle(_stopEvent);
                _stopEvent = nullptr;
            }
        }

        // Runs the main function for a service that lives in its own process.
        static HRESULT ProcessMain()
        {
            const SERVICE_TABLE_ENTRY DispatchTable[] = {
                {const_cast<LPWSTR>(L""),
                 (LPSERVICE_MAIN_FUNCTION)&Service<TBase, TLastObjectReleaseBehavior, TSecurityPolicy, TExceptionPolicy>::SvcMain},
                {nullptr, nullptr}};

            RETURN_IF_WIN32_BOOL_FALSE(StartServiceCtrlDispatcher(DispatchTable));

            return s_LastServiceMainHR;
        }

        // Runs the service itself. Only necessary when ProcessMain isn't used.
        static void ServiceMainSharedProcess()
        {
            TBase instance;
            s_LastServiceMainHR = instance.RunServiceMain(false);
        }

        HRESULT RunServiceMain(_In_ boolean fOwnProcess)
        {
            __if_exists(TBase::ServiceStopped)
            {
                _serviceStatus.dwControlsAccepted |= SERVICE_ACCEPT_STOP;
            }

            __if_exists(TBase::OnSystemShutdown)
            {
                _serviceStatus.dwControlsAccepted |= SERVICE_ACCEPT_SHUTDOWN;
            }

            __if_exists(TBase::OnSessionChanged)
            {
                _serviceStatus.dwControlsAccepted |= SERVICE_ACCEPT_SESSIONCHANGE;
            }

            ServiceModuleBase* pModule = nullptr;
            HRESULT hr = [&]() {
                // The service handle need not be closed.
                _serviceStatusHandle = RegisterServiceCtrlHandlerEx(TBase::GetName(), &Service::HandlerExStatic, this);
                RETURN_LAST_ERROR_IF(_serviceStatusHandle == 0);

                _stopEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
                RETURN_LAST_ERROR_IF(_stopEvent == nullptr);

                __if_exists(TBase::OnServiceStarting)
                {
                    RETURN_IF_FAILED(reinterpret_cast<TBase*>(this)->OnServiceStarting());
                }

                if (fOwnProcess)
                {
                    pModule = &(ServiceModule::Create(this, GetModuleCallback<TLastObjectReleaseBehavior>()));
                }
                else
                {
                    RETURN_HR(E_NOTIMPL);
                }

                constexpr bool addModuleReference = (TLastObjectReleaseBehavior == ContinueRunningWithNoObjects);

                RETURN_IF_FAILED((pModule->Initialize<TSecurityPolicy, TExceptionPolicy, TServerDescriptor>(
                    fOwnProcess, addModuleReference, true /*hasDedicatedThread*/, _stopEvent)));

                RETURN_IF_FAILED(hr = reinterpret_cast<TBase*>(this)->ServiceStarted());
                auto serviceStopped = wil::scope_exit([&] {
                    __if_exists(TBase::ServiceStopped)
                    {
                        reinterpret_cast<TBase*>(this)->ServiceStopped();
                    }
                });

                __if_exists(TBase::OnLowPowerModeChanged)
                {
                    RETURN_IF_WIN32_ERROR(PowerSettingRegisterNotification(
                        &GUID_LOW_POWER_EPOCH_PRV, DEVICE_NOTIFY_SERVICE_HANDLE, _serviceStatusHandle, &_powerHandle));
                }

                __if_exists(TBase::OnLowPowerModeChanged)
                {
                    _serviceStatus.dwControlsAccepted |= SERVICE_ACCEPT_POWEREVENT;
                }

                ReportCurrentStatus();
                WaitForSingleObject(_stopEvent, INFINITE);

                // The service is stopping now.
                serviceStopped.reset();

                __if_exists(TBase::OnLowPowerModeChanged)
                {
                    if (_powerHandle != nullptr)
                    {
                        PowerSettingUnregisterNotification(_powerHandle);
                        _powerHandle = nullptr;
                    }
                }

                return S_OK;
            }();

            //
            // See http://blogs.msdn.com/b/oldnewthing/archive/2006/11/03/942851.aspx for
            // a discussion on why this is lossy.
            //
            if (HRESULT_FACILITY(hr) == FACILITY_WIN32)
            {
                _serviceStatus.dwWin32ExitCode = HRESULT_CODE(hr);
            }
            else
            {
                if (FAILED(hr))
                {
                    _serviceStatus.dwWin32ExitCode = ERROR_SERVICE_SPECIFIC_ERROR;
                }
                _serviceStatus.dwServiceSpecificExitCode = hr;
            }

            // Unregister the COM objects if the service module was created.
            if (pModule != nullptr)
            {
                pModule->Uninitialize();
            }

            _serviceStatus.dwCurrentState = SERVICE_STOPPED;
            ReportCurrentStatus();

            RETURN_IF_FAILED(hr);

            return S_OK;
        }

        // Returns the service status handle for this service.
        SERVICE_STATUS_HANDLE GetServiceStatusHandle() const
        {
            return _serviceStatusHandle;
        }

    protected:
        // Reports the current status information.
        void ReportCurrentStatus()
        {
            SetServiceStatus(_serviceStatusHandle, &_serviceStatus);
        }

        // Gets a mutable reference to the current status information.
        LPSERVICE_STATUS GetServiceStatusReference()
        {
            return &_serviceStatus;
        }

        // Asynchronously stops this service, typically in response to a SERVICE_CONTROL_STOP request.
        void StopAsync()
        {
            if (_serviceStatus.dwCurrentState != SERVICE_STOP_PENDING && _serviceStatus.dwCurrentState != SERVICE_STOPPED)
            {
                _serviceStatus.dwCurrentState = SERVICE_STOP_PENDING;
                ReportCurrentStatus();
            }
            SetEvent(_stopEvent);
        }

        // Asynchronously stops this service, typically in response to an async initialization issue
        void StopAsync(HRESULT hr)
        {
            if (hr != S_OK)
            {
                if (HRESULT_FACILITY(hr) == FACILITY_WIN32)
                {
                    _serviceStatus.dwWin32ExitCode = HRESULT_CODE(hr);
                }
                else
                {
                    if (FAILED(hr))
                    {
                        _serviceStatus.dwWin32ExitCode = ERROR_SERVICE_SPECIFIC_ERROR;
                    }
                    _serviceStatus.dwServiceSpecificExitCode = hr;
                }
            }

            StopAsync();
        }

    private:
        static void SvcMain(DWORD, LPWSTR*)
        {
            TBase instance;
            s_LastServiceMainHR = instance.RunServiceMain(true);
        }

        static DWORD WINAPI HandlerExStatic(_In_ DWORD dwControl, _In_ DWORD dwEventType, _In_ LPVOID lpEventData, _In_ LPVOID lpContext)
        {
            Service* self = reinterpret_cast<Service*>(lpContext);
            return self->HandlerEx(dwControl, dwEventType, lpEventData);
        }

        DWORD WINAPI HandlerEx(_In_ DWORD dwControl, _In_ DWORD dwEventType, _In_ LPVOID lpEventData)
        {
            // Unreferenced when OnLowPowerModeChanged isn't defined, but this won't hurt.
            UNREFERENCED_PARAMETER(dwEventType);
            UNREFERENCED_PARAMETER(lpEventData);

            DWORD dwResult = ERROR_CALL_NOT_IMPLEMENTED;

            __if_exists(TBase::OnHandlerEx)
            {
                dwResult = reinterpret_cast<TBase*>(this)->OnHandlerEx(dwControl, dwEventType, lpEventData);
            }

            // See http://msdn.microsoft.com/en-us/library/windows/desktop/ms683241(v=vs.85).aspx for codes.
            if (dwControl == SERVICE_CONTROL_STOP)
            {
                StopAsync();
            }

            // Provide first-class support for lower power mode when OnLowPowerModeChanged is available.
            // Additional support can be implemented by overriding OnHandlerEx.
            __if_exists(TBase::OnLowPowerModeChanged)
            {
                if (dwControl == SERVICE_CONTROL_POWEREVENT)
                {
                    PPOWERBROADCAST_SETTING powerSetting;
                    switch (dwEventType)
                    {
                    case PBT_POWERSETTINGCHANGE:
                        powerSetting = static_cast<PPOWERBROADCAST_SETTING>(lpEventData);
                        if (!memcmp(&powerSetting->PowerSetting, &GUID_LOW_POWER_EPOCH_PRV, sizeof(powerSetting->PowerSetting)) &&
                            powerSetting->DataLength == sizeof(ULONG))
                        {
                            switch (*reinterpret_cast<ULONG*>(powerSetting->Data))
                            {
                            case 0:
                                // Exiting lower power mode change.
                                reinterpret_cast<TBase*>(this)->OnLowPowerModeChanged(false);
                                dwResult = NO_ERROR;
                                break;

                            case 1:
                                // Entering lower power mode change.
                                reinterpret_cast<TBase*>(this)->OnLowPowerModeChanged(true);
                                dwResult = NO_ERROR;
                                break;
                            }
                        }
                        break;
                    case PBT_APMPOWERSTATUSCHANGE:
                    case PBT_APMRESUMEAUTOMATIC:
                    case PBT_APMSUSPEND:
                    default:
                        break;
                    }
                }
            }

            __if_exists(TBase::OnSessionChanged)
            {
                if (dwControl == SERVICE_CONTROL_SESSIONCHANGE)
                {
                    PWTSSESSION_NOTIFICATION sessionNotification;
                    sessionNotification = static_cast<PWTSSESSION_NOTIFICATION>(lpEventData);
                    reinterpret_cast<TBase*>(this)->OnSessionChanged(dwEventType, sessionNotification->dwSessionId);
                    dwResult = NO_ERROR;
                }
            }

            // Provide first-class support for system shutdown when OnSystemShutdown is available.
            __if_exists(TBase::OnSystemShutdown)
            {
                if (dwControl == SERVICE_CONTROL_SHUTDOWN)
                {
                    //
                    // If a service accepts this control code, it must stop
                    // after it performs its cleanup tasks and return NO_ERROR.
                    // After the SCM sends this control code, it will not send other
                    // control codes to the service.
                    //
                    // We stop asynchronously to have the same codepath as system
                    // stop requests.
                    //
                    reinterpret_cast<TBase*>(this)->OnSystemShutdown();
                    StopAsync();
                    dwResult = NO_ERROR;
                }
            }

            return (dwControl == SERVICE_CONTROL_STOP || dwControl == SERVICE_CONTROL_INTERROGATE) ? NO_ERROR : dwResult;
        }

        typedef void (Service::*CallbackFn)();

        template <LastObjectReleaseBehavior>
        CallbackFn GetModuleCallback();

        template <>
        CallbackFn GetModuleCallback<ShutdownAfterLastObjectReleased>()
        {
            return &Service::StopAsync;
        }

        template <>
        CallbackFn GetModuleCallback<ContinueRunningWithNoObjects>()
        {
            return &Service::DummyNoOpCallback;
        }

        void DummyNoOpCallback()
        {
        }

        // HRESULT of the last service main call. Only used when ProcessMain is called.
        static HRESULT s_LastServiceMainHR;

        // Don't force all callers to adjust project include paths for a single constant.
        static const GUID GUID_LOW_POWER_EPOCH_PRV;

        // A handle to the power registration for low power epoch.
        HPOWERNOTIFY _powerHandle = nullptr;

        // Handle to identify this service instance.
        SERVICE_STATUS_HANDLE _serviceStatusHandle = nullptr;

        // Structure used to report service status updates.
        SERVICE_STATUS _serviceStatus{};

        // Event object to signal that the service should stop.
        HANDLE _stopEvent = nullptr;
    };

    template <typename TBase, LastObjectReleaseBehavior TLastObjectReleaseBehavior, typename TSecurityPolicy, GLOBALOPT_EH_VALUES TExceptionPolicy, typename TServerDescriptor>
    __declspec(selectany) HRESULT Service<TBase, TLastObjectReleaseBehavior, TSecurityPolicy, TExceptionPolicy, TServerDescriptor>::s_LastServiceMainHR;

    template <typename TBase, LastObjectReleaseBehavior TLastObjectReleaseBehavior, typename TSecurityPolicy, GLOBALOPT_EH_VALUES TExceptionPolicy, typename TServerDescriptor>
    __declspec(selectany)
    const GUID Service<TBase, TLastObjectReleaseBehavior, TSecurityPolicy, TExceptionPolicy, TServerDescriptor>::GUID_LOW_POWER_EPOCH_PRV = {
        0xe1233993, 0xeaa4, 0x470f, {0x9d, 0xe7, 0xa3, 0x51, 0xc1, 0xb6, 0xfb, 0x71}};

}} // namespace Windows::Internal

// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include "UsbDevice.h"
#include "UsbTypes.h"
#include "UsbUxdRegistry.h"
#include <cfgmgr32.h>
#include <wil/resource.h>
#include <atomic>
#include <list>
#include <map>
#include <thread>

namespace wsl::windows::common::usb {

class Manager
{
public:
    Manager();
    ~Manager();
    NON_COPYABLE(Manager);
    NON_MOVABLE(Manager);

    std::vector<ListedDevice> List(PSID UserSid);
    void Attach(PSID UserSid, PortSelector Port);
    void Detach(PSID UserSid, PortSelector Port);
    void VmStarted(PSID UserSid, const GUID& VmId);
    void VmStopped(PSID UserSid) noexcept;
    void ShutdownByUser(PSID UserSid) noexcept;
    void Stop() noexcept;

private:
    // Records include pending attachments and incomplete cleanup.
    // Active permits replay; it does not require a running VM.
    // VmId tracks the VM binding independently; GUID_NULL means no recorded binding.
    struct Record
    {
        UxdReservationIdentity Reservation;
        std::wstring OwnerSid;
        std::wstring Name;
        std::wstring DisplayInstanceId;
        std::wstring InterfacePath;
        std::wstring InstanceId;
        GUID VmId{};
        bool Active = false;
    };

    struct Removal
    {
        std::wstring Identifier;
        bool Instance;
    };

    static DWORD CALLBACK OnDeviceNotification(HCMNOTIFICATION, PVOID Context, CM_NOTIFY_ACTION Action, PCM_NOTIFY_EVENT_DATA Data, DWORD) noexcept;
    static std::wstring ConvertSidToString(PSID Sid);
    Inventory RefreshDeviceInventory();
    UxdReservationIdentity ResolvePortSelector(PortSelector Port) const;
    uint32_t GetHubNumber(const std::wstring& InstanceId) const;
    std::list<Record>::iterator FindReservationRecord(const UxdReservationIdentity& Value);
    std::optional<Device> FindDeviceAtPort(const Inventory& Current, const UxdReservationIdentity& Value) const;
    bool MatchesRecordedReservation(const Record& Value) const;
    Device WaitForWinUsbDevice(const UxdReservationIdentity& Value, std::wstring& Path);
    void LoadReservationRecords();
    void PersistReservationRecord(const Record& Value);
    void ReleaseReservation(Record& Value);
    void QueueDeviceRemoval(std::wstring Identifier, bool Instance);
    void ProcessDeviceRemovals();
    void AttachDeviceToVm(Record& Value);
    void DetachDeviceFromVm(Record& Value);
    void ThrowIfStopping() const;
    void RunNotificationWorker() noexcept;

    wil::srwlock m_lock;
    _Guarded_by_(m_lock) std::list<Record> m_records;
    _Guarded_by_(m_lock) std::vector<std::wstring> m_hubs;
    _Guarded_by_(m_lock) std::map<std::wstring, GUID> m_vms;
    UxdRegistry m_registry;
    wil::unique_hkey m_journal;
    wil::unique_event m_stop;
    wil::unique_event m_changed;
    wil::unique_event m_work;
    wil::srwlock m_notificationLock;
    _Guarded_by_(m_notificationLock) std::vector<Removal> m_removals;
    std::atomic<bool> m_stopping{false};
    using NotificationHandle = wil::unique_any<HCMNOTIFICATION, decltype(&CM_Unregister_Notification), CM_Unregister_Notification>;
    NotificationHandle m_interfaceNotification;
    NotificationHandle m_instanceNotification;
    std::thread m_worker;
};

} // namespace wsl::windows::common::usb
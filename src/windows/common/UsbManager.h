// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include "UsbDevice.h"
#include "UsbTypes.h"
#include "UsbUxdRegistry.h"
#include <cfgmgr32.h>
#include <wil/resource.h>
#include <atomic>
#include <functional>
#include <list>
#include <thread>

namespace wsl::windows::common::usb {

class Manager
{
public:
    using RemovalCallback = std::function<void(const GUID&)>;

    Manager();
    ~Manager();
    NON_COPYABLE(Manager);
    NON_MOVABLE(Manager);

    std::vector<ListedDevice> List(PSID UserSid);
    void Stop() noexcept;

    DeviceReservation Reserve(PSID UserSid, const PortId& Port, RemovalCallback OnRemoval);
    void Commit(PSID UserSid, const GUID& ReservationId);
    void Release(PSID UserSid, const GUID& ReservationId);
    GUID GetReservationId(PSID UserSid, const PortId& Port);
    bool IsValid(PSID UserSid, const GUID& ReservationId);
    void Cancel() noexcept;

private:
    // Records include pending attachments and incomplete cleanup.
    // Active identifies reservations committed by their owner.
    struct Record
    {
        UxdReservationIdentity Reservation;
        std::wstring OwnerSid;
        std::wstring Name;
        std::wstring DisplayInstanceId;
        std::wstring InterfacePath;
        std::wstring InstanceId;
        RemovalCallback OnRemoval;
        bool Active = false;
        bool Removed = false;
    };

    struct Removal
    {
        std::wstring Identifier;
        bool Instance;
    };

    static DWORD CALLBACK OnDeviceNotification(HCMNOTIFICATION, PVOID Context, CM_NOTIFY_ACTION Action, PCM_NOTIFY_EVENT_DATA Data, DWORD) noexcept;
    static std::wstring ConvertSidToString(PSID Sid);
    UxdReservationIdentity ResolvePortId(const Inventory& Current, const PortId& Port) const;
    std::list<Record>::iterator FindReservationRecord(const UxdReservationIdentity& Value);
    std::optional<Device> FindDeviceAtPort(const Inventory& Current, const UxdReservationIdentity& Value) const;
    bool MatchesRecordedReservation(const Record& Value) const;
    Device WaitForWinUsbDevice(const UxdReservationIdentity& Value, std::wstring& Path);
    void LoadReservationRecords();
    void PersistReservationRecord(const Record& Value);
    void ReleaseReservation(Record& Value);
    void QueueDeviceRemoval(std::wstring Identifier, bool Instance);
    void ProcessDeviceRemovals();
    void ThrowIfStopping() const;
    void RunNotificationWorker() noexcept;
    std::list<Record>::iterator FindReservationRecord(const GUID& ReservationId);
    bool ValidateReservation(Record& Value);

    wil::srwlock m_lock;
    _Guarded_by_(m_lock) std::list<Record> m_records;
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
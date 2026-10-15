// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "UsbManager.h"
#include "stringshared.h"
#include <limits>
#include <nlohmann/json.hpp>
#include <usbiodef.h>

namespace wsl::windows::common::usb {

using wsl::shared::string::IsEqual;
using wsl::shared::string::MultiByteToWide;
using wsl::shared::string::WideToMultiByte;

namespace {

    constexpr wchar_t c_journalPath[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Lxss\\Usb";
    constexpr DWORD c_readyTimeoutMs = 30000;

    std::wstring GetJournalRecordName(const UxdReservationIdentity& Value)
    {
        return wsl::shared::string::GuidToString<wchar_t>(Value.Id);
    }

    wil::unique_hkey CreateJournalKey()
    {
        auto key = registry::CreateKey(HKEY_LOCAL_MACHINE, c_journalPath, KEY_READ | KEY_WRITE | WRITE_DAC);

        // Restrict ownership records to SYSTEM and administrators.
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        THROW_IF_WIN32_BOOL_FALSE(ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:P(A;;KA;;;SY)(A;;KA;;;BA)", SDDL_REVISION_1, &descriptor, nullptr));
        wil::unique_hlocal security(descriptor);
        THROW_IF_WIN32_ERROR(
            RegSetKeySecurity(key.get(), DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, security.get()));
        return key;
    }

    void MockAttachDeviceToVm(const GUID& VmId, const std::wstring& Path)
    {
        // TODO: Replace this logging-only success placeholder with the VM USB attach API once it is implemented.
        WSL_LOG("UsbAttachDeviceMock", TraceLoggingGuid(VmId, "VmId"), TraceLoggingWideString(Path.c_str(), "DevicePath"));
    }

    void MockDetachDeviceFromVm(const GUID& VmId, const std::wstring& Path)
    {
        // TODO: Replace this logging-only success placeholder with the VM USB detach API once it is implemented.
        WSL_LOG("UsbDetachDeviceMock", TraceLoggingGuid(VmId, "VmId"), TraceLoggingWideString(Path.c_str(), "DevicePath"));
    }

} // namespace

PortSelector ParsePortSelector(std::wstring_view Value)
{
    const auto separator = Value.find(L'-');
    THROW_HR_IF(E_INVALIDARG, separator == std::wstring_view::npos);

    const auto parse = [](std::wstring_view text, uint32_t maximum) {
        THROW_HR_IF(E_INVALIDARG, text.empty());
        uint32_t result = 0;
        for (const auto digit : text)
        {
            THROW_HR_IF(E_INVALIDARG, digit < L'0' || digit > L'9');
            const auto number = static_cast<uint32_t>(digit - L'0');
            THROW_HR_IF(E_INVALIDARG, result > (maximum - number) / 10);
            result = result * 10 + number;
        }

        THROW_HR_IF(E_INVALIDARG, result == 0 || result > maximum);
        return result;
    };

    return {parse(Value.substr(0, separator), std::numeric_limits<uint32_t>::max()), parse(Value.substr(separator + 1), 999)};
}

Manager::Manager() : m_journal(CreateJournalKey())
{
    m_stop.create(wil::EventOptions::ManualReset);
    m_changed.create();
    m_work.create();

    // Recover and release reservations left by a previous service run.
    {
        auto lock = m_lock.lock_exclusive();
        LoadReservationRecords();

        for (auto record = m_records.begin(); record != m_records.end();)
        {
            try
            {
                ReleaseReservation(*record);
                record = m_records.erase(record);
            }
            catch (...)
            {
                // Keep failed cleanup records available for later recovery.
                LOG_CAUGHT_EXCEPTION();
                ++record;
            }
        }
    }

    // Watch USB interfaces for arrivals and removals.
    CM_NOTIFY_FILTER filter{};
    filter.cbSize = sizeof(filter);
    filter.FilterType = CM_NOTIFY_FILTER_TYPE_DEVICEINTERFACE;
    filter.u.DeviceInterface.ClassGuid = GUID_DEVINTERFACE_USB_DEVICE;
    THROW_IF_WIN32_ERROR(CM_MapCrToWin32Err(
        CM_Register_Notification(&filter, this, OnDeviceNotification, m_interfaceNotification.put()), ERROR_GEN_FAILURE));

    // Watch instance events for readiness and removal; the callback filters out non-USB devices.
    filter = {};
    filter.cbSize = sizeof(filter);
    filter.FilterType = CM_NOTIFY_FILTER_TYPE_DEVICEINSTANCE;
    filter.Flags = CM_NOTIFY_FILTER_FLAG_ALL_DEVICE_INSTANCES;
    THROW_IF_WIN32_ERROR(CM_MapCrToWin32Err(
        CM_Register_Notification(&filter, this, OnDeviceNotification, m_instanceNotification.put()), ERROR_GEN_FAILURE));

    // Keep removal cleanup out of PnP notification callbacks.
    m_worker = std::thread([this]() { RunNotificationWorker(); });
}

Manager::~Manager()
{
    Stop();
}

std::wstring Manager::ConvertSidToString(PSID Sid)
{
    THROW_HR_IF(E_INVALIDARG, Sid == nullptr || !IsValidSid(Sid));
    wil::unique_hlocal_string text;
    THROW_IF_WIN32_BOOL_FALSE(ConvertSidToStringSidW(Sid, text.put()));
    return text.get();
}

void Manager::ThrowIfStopping() const
{
    THROW_HR_IF(CO_E_SERVER_STOPPING, m_stopping.load());
}

Inventory Manager::RefreshDeviceInventory()
{
    auto inventory = Inventory::Enumerate();
    std::ranges::sort(inventory.Hubs, {}, &Hub::InstanceId);

    // Keep existing hub numbers stable for the lifetime of the manager.
    for (const auto& hub : inventory.Hubs)
    {
        if (!std::ranges::any_of(m_hubs, [&](const auto& id) { return IsEqual(id, hub.InstanceId, true); }))
        {
            m_hubs.push_back(hub.InstanceId);
        }
    }

    return inventory;
}

uint32_t Manager::GetHubNumber(const std::wstring& InstanceId) const
{
    const auto found = std::ranges::find_if(m_hubs, [&](const auto& id) { return IsEqual(id, InstanceId, true); });
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_NOT_FOUND), found == m_hubs.end());
    return gsl::narrow_cast<uint32_t>(std::distance(m_hubs.begin(), found) + 1);
}

UxdReservationIdentity Manager::ResolvePortSelector(PortSelector Port) const
{
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_NOT_FOUND), Port.Hub == 0 || Port.Hub > m_hubs.size() || Port.Port == 0 || Port.Port > 999);
    return {m_hubs[Port.Hub - 1], Port.Port, {}};
}

std::list<Manager::Record>::iterator Manager::FindReservationRecord(const UxdReservationIdentity& Value)
{
    return std::ranges::find_if(m_records, [&](const auto& record) {
        return record.Reservation.Port == Value.Port && IsEqual(record.Reservation.HubInstanceId, Value.HubInstanceId, true);
    });
}

std::optional<Device> Manager::FindDeviceAtPort(const Inventory& Current, const UxdReservationIdentity& Value) const
{
    for (const auto& device : Current.Devices)
    {
        if (device.Port == Value.Port && IsEqual(device.HubInstanceId, Value.HubInstanceId, true))
        {
            return device;
        }
    }
    return {};
}

bool Manager::MatchesRecordedReservation(const Record& Value) const
{
    const auto reservation = m_registry.ReadReservation(Value.Reservation);
    return reservation && OwnsReservation(Value.Reservation.Id, *reservation) && m_registry.MappingMatches(Value.Reservation);
}

std::vector<ListedDevice> Manager::List(PSID UserSid)
{
    const auto sid = ConvertSidToString(UserSid);
    auto lock = m_lock.lock_exclusive();
    ThrowIfStopping();

    ProcessDeviceRemovals();
    const auto inventory = RefreshDeviceInventory();

    std::vector<ListedDevice> result;
    for (const auto& device : inventory.Devices)
    {
        const UxdReservationIdentity identity{device.HubInstanceId, device.Port, {}};
        const auto record = FindReservationRecord(identity);
        const bool reserved = record != m_records.end() || m_registry.HasReservation(identity) || device.IsRedirected();

        // Revalidate ownership before reporting a device as attached.
        std::wstring owner;
        if (record != m_records.end() && record->Active)
        {
            try
            {
                if (MatchesRecordedReservation(*record))
                {
                    owner = record->OwnerSid;
                }
            }
            CATCH_LOG()
        }

        result.push_back(
            {std::format(L"{}-{}", GetHubNumber(device.HubInstanceId), device.Port),
             record == m_records.end() || record->Name.empty() ? device.Name : record->Name,
             record == m_records.end() || record->DisplayInstanceId.empty() ? device.DisplayInstanceId : record->DisplayInstanceId,
             reserved ? (owner == sid ? DeviceState::Attached : DeviceState::Unavailable) : DeviceState::Unattached});
    }
    return result;
}

Device Manager::WaitForWinUsbDevice(const UxdReservationIdentity& Value, std::wstring& Path)
{
    const auto deadline = GetTickCount64() + c_readyTimeoutMs;
    for (;;)
    {
        ThrowIfStopping();

        // Revalidate ownership before probing the redirected interface.
        const auto reservation = m_registry.ReadReservation(Value);
        THROW_HR_IF(E_CHANGED_STATE, !reservation || !OwnsReservation(Value.Id, *reservation));

        const auto current = FindDeviceAtPort(RefreshDeviceInventory(), Value);
        if (current)
        {
            if (auto path = current->ProbeWinUsb())
            {
                Path = std::move(*path);
                return *current;
            }
        }

        // Wait for a device change or cancellation, up to the readiness deadline.
        const auto now = GetTickCount64();
        THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_TIMEOUT), now >= deadline);
        const HANDLE events[] = {m_stop.get(), m_changed.get()};
        const auto wait = WaitForMultipleObjects(ARRAYSIZE(events), events, FALSE, gsl::narrow_cast<DWORD>(deadline - now));
        THROW_LAST_ERROR_IF(wait == WAIT_FAILED);
        THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_CANCELLED), wait == WAIT_OBJECT_0);
    }
}

void Manager::Attach(PSID UserSid, PortSelector Port)
{
    const auto sid = ConvertSidToString(UserSid);
    auto lock = m_lock.lock_exclusive();
    ThrowIfStopping();

    // Validate the selected device and reject existing reservations.
    ProcessDeviceRemovals();
    const auto inventory = RefreshDeviceInventory();
    auto identity = ResolvePortSelector(Port);
    const auto existing = FindReservationRecord(identity);
    if (existing != m_records.end())
    {
        THROW_HR_WITH_USER_ERROR_IF(E_ACCESSDENIED, wsl::shared::Localization::MessageUsbUnavailable(), existing->OwnerSid != sid);
        THROW_HR_WITH_USER_ERROR_IF(
            HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS), wsl::shared::Localization::MessageUsbAlreadyAttached(), existing->Active);
        THROW_HR_WITH_USER_ERROR(HRESULT_FROM_WIN32(ERROR_BUSY), wsl::shared::Localization::MessageUsbUnavailable());
    }

    const auto source = FindDeviceAtPort(inventory, identity);
    THROW_HR_WITH_USER_ERROR_IF(HRESULT_FROM_WIN32(ERROR_NOT_FOUND), wsl::shared::Localization::MessageUsbNotFound(), !source);
    THROW_HR_WITH_USER_ERROR_IF(
        HRESULT_FROM_WIN32(ERROR_BUSY),
        wsl::shared::Localization::MessageUsbUnavailable(),
        m_registry.HasReservation(identity) || source->IsRedirected());

    THROW_IF_FAILED(CoCreateGuid(&identity.Id));
    Record value;
    value.Reservation = identity;
    value.OwnerSid = sid;
    value.Name = source->Name;
    value.DisplayInstanceId = source->DisplayInstanceId;
    m_records.push_back(std::move(value));
    const auto record = std::prev(m_records.end());

    bool persisted = false;
    try
    {
        // Make ownership durable before publishing redirection state.
        PersistReservationRecord(*record);
        persisted = true;
        m_registry.CreateMapping(identity);
        m_registry.PublishReservation(identity);

        // Reload the port and grant the owner access to the redirected interface.
        CyclePort(identity.HubInstanceId, identity.Port);
        const auto redirected = WaitForWinUsbDevice(identity, record->InterfacePath);
        record->InstanceId = redirected.InstanceId;
        GrantDeviceAccess(record->InterfacePath, UserSid);

        // Activate the reservation and bind it if the user's VM is running.
        record->Active = true;
        AttachDeviceToVm(*record);
    }
    catch (...)
    {
        const auto error = wil::ResultFromCaughtException();

        // Roll back partial redirection; keep the record if cleanup fails.
        try
        {
            if (persisted)
            {
                ReleaseReservation(*record);
            }
            m_records.erase(record);
        }
        CATCH_LOG_MSG("Failed to roll back USB attachment after error 0x%08X", error)
        THROW_HR(error);
    }
}

void Manager::Detach(PSID UserSid, PortSelector Port)
{
    const auto sid = ConvertSidToString(UserSid);
    auto lock = m_lock.lock_exclusive();
    ThrowIfStopping();

    ProcessDeviceRemovals();
    RefreshDeviceInventory();

    const auto record = FindReservationRecord(ResolvePortSelector(Port));
    THROW_HR_WITH_USER_ERROR_IF(
        HRESULT_FROM_WIN32(ERROR_NOT_FOUND), wsl::shared::Localization::MessageUsbNotAttached(), record == m_records.end());
    THROW_HR_WITH_USER_ERROR_IF(E_ACCESSDENIED, wsl::shared::Localization::MessageUsbUnavailable(), record->OwnerSid != sid);

    // Return the device to Windows before forgetting its ownership record.
    ReleaseReservation(*record);
    m_records.erase(record);
}

void Manager::AttachDeviceToVm(Record& Value)
{
    const auto vm = m_vms.find(Value.OwnerSid);
    if (Value.Active && vm != m_vms.end() && IsEqualGUID(Value.VmId, GUID_NULL))
    {
        MockAttachDeviceToVm(vm->second, Value.InterfacePath);
        Value.VmId = vm->second;
    }
}

void Manager::DetachDeviceFromVm(Record& Value)
{
    if (!IsEqualGUID(Value.VmId, GUID_NULL))
    {
        MockDetachDeviceFromVm(Value.VmId, Value.InterfacePath);
        Value.VmId = GUID_NULL;
    }
}

void Manager::VmStarted(PSID UserSid, const GUID& VmId)
{
    const auto sid = ConvertSidToString(UserSid);
    auto lock = m_lock.lock_exclusive();
    ThrowIfStopping();
    ProcessDeviceRemovals();

    // Attaching another device to the same VM must not replay existing reservations.
    const auto existing = m_vms.find(sid);
    if (existing != m_vms.end() && IsEqualGUID(existing->second, VmId))
    {
        return;
    }

    // Revalidate and replay this user's active reservations into the new VM.
    m_vms[sid] = VmId;
    for (auto record = m_records.begin(); record != m_records.end();)
    {
        if (record->OwnerSid != sid || !record->Active)
        {
            ++record;
            continue;
        }

        // Release only confirmed mismatches; read or replay failures retain the record.
        try
        {
            if (!MatchesRecordedReservation(*record))
            {
                ReleaseReservation(*record);
                record = m_records.erase(record);
                continue;
            }

            AttachDeviceToVm(*record);
        }
        CATCH_LOG()
        ++record;
    }
}

void Manager::VmStopped(PSID UserSid) noexcept
try
{
    const auto sid = ConvertSidToString(UserSid);
    auto lock = m_lock.lock_exclusive();

    // Preserve Windows reservations while forgetting bindings to the stopped VM.
    m_vms.erase(sid);
    for (auto& record : m_records)
    {
        if (record.OwnerSid == sid)
        {
            try
            {
                DetachDeviceFromVm(record);
            }
            CATCH_LOG()
            record.VmId = GUID_NULL;
        }
    }
}
CATCH_LOG()

void Manager::ShutdownByUser(PSID UserSid) noexcept
try
{
    const auto sid = ConvertSidToString(UserSid);
    auto lock = m_lock.lock_exclusive();

    // Explicit shutdown releases this user's reservations back to Windows.
    m_vms.erase(sid);
    for (auto record = m_records.begin(); record != m_records.end();)
    {
        if (record->OwnerSid != sid)
        {
            ++record;
            continue;
        }
        try
        {
            ReleaseReservation(*record);
            record = m_records.erase(record);
        }
        catch (...)
        {
            LOG_CAUGHT_EXCEPTION();
            ++record;
        }
    }
}
CATCH_LOG()

void Manager::LoadReservationRecords()
{
    // Restore ownership metadata for cleanup, without reactivating attachments.
    for (const auto& [name, type] : registry::EnumValues(m_journal.get()))
    {
        try
        {
            THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_DATA), type != REG_SZ);
            Record record;
            THROW_IF_FAILED(CLSIDFromString(name.c_str(), &record.Reservation.Id));
            THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_DATA), IsEqualGUID(record.Reservation.Id, GUID_NULL));

            const auto json = nlohmann::json::parse(WideToMultiByte(registry::ReadString(m_journal.get(), nullptr, name.c_str())));
            record.Reservation.HubInstanceId = MultiByteToWide(json.at("Hub").get<std::string>());
            record.Reservation.Port = json.at("Port").get<uint32_t>();
            record.OwnerSid = MultiByteToWide(json.at("OwnerSid").get<std::string>());

            m_records.push_back(std::move(record));
        }
        CATCH_LOG()
    }
}

void Manager::PersistReservationRecord(const Record& Value)
{
    const nlohmann::json record{
        {"Hub", WideToMultiByte(Value.Reservation.HubInstanceId)},
        {"Port", Value.Reservation.Port},
        {"OwnerSid", WideToMultiByte(Value.OwnerSid)}};

    const auto text = MultiByteToWide(record.dump());
    THROW_IF_WIN32_ERROR(RegSetValueExW(
        m_journal.get(),
        GetJournalRecordName(Value.Reservation).c_str(),
        0,
        REG_SZ,
        reinterpret_cast<const BYTE*>(text.c_str()),
        gsl::narrow_cast<DWORD>((text.size() + 1) * sizeof(wchar_t))));

    THROW_IF_WIN32_ERROR(RegFlushKey(m_journal.get()));
}

void Manager::ReleaseReservation(Record& Value)
{
    // Disable replay and drop any VM binding before restoring Windows ownership.
    Value.Active = false;
    DetachDeviceFromVm(Value);

    // Leave reservations belonging to other owners untouched.
    const auto reservation = m_registry.ReadReservation(Value.Reservation);
    if (!reservation || OwnsReservation(Value.Reservation.Id, *reservation))
    {
        const auto current = FindDeviceAtPort(RefreshDeviceInventory(), Value.Reservation);
        const bool removed = m_registry.RemoveReservation(Value.Reservation);
        if (current && (removed || current->IsRedirected()))
        {
            CyclePort(Value.Reservation.HubInstanceId, Value.Reservation.Port);
        }
    }

    // Make SYSTEM cleanup durable before deleting the SOFTWARE ownership record.
    m_registry.RemoveMapping(Value.Reservation);
    m_registry.Flush();

    const auto error = RegDeleteValueW(m_journal.get(), GetJournalRecordName(Value.Reservation).c_str());
    THROW_IF_WIN32_ERROR(error == ERROR_FILE_NOT_FOUND ? ERROR_SUCCESS : error);
    THROW_IF_WIN32_ERROR(RegFlushKey(m_journal.get()));
}

void Manager::QueueDeviceRemoval(std::wstring Identifier, bool Instance)
{
    // Queue removal work without taking the main manager lock.
    {
        auto lock = m_notificationLock.lock_exclusive();
        m_removals.push_back({std::move(Identifier), Instance});
    }

    SetEvent(m_work.get());
    SetEvent(m_changed.get());
}

void Manager::ProcessDeviceRemovals()
{
    std::vector<Removal> removals;
    {
        auto lock = m_notificationLock.lock_exclusive();
        removals.swap(m_removals);
    }

    // Release only reservations matched by received removal notifications.
    for (auto record = m_records.begin(); record != m_records.end();)
    {
        const auto removed = std::ranges::any_of(removals, [&](const auto& removal) {
            return IsEqual(removal.Identifier, removal.Instance ? record->InstanceId : record->InterfacePath, true);
        });
        if (!removed)
        {
            ++record;
            continue;
        }
        try
        {
            ReleaseReservation(*record);
            record = m_records.erase(record);
        }
        catch (...)
        {
            LOG_CAUGHT_EXCEPTION();
            ++record;
        }
    }
}

DWORD CALLBACK Manager::OnDeviceNotification(HCMNOTIFICATION, PVOID Context, CM_NOTIFY_ACTION Action, PCM_NOTIFY_EVENT_DATA Data, DWORD) noexcept
{
    auto& manager = *static_cast<Manager*>(Context);
    const bool instance = Action == CM_NOTIFY_ACTION_DEVICEINSTANCEENUMERATED ||
                          Action == CM_NOTIFY_ACTION_DEVICEINSTANCESTARTED || Action == CM_NOTIFY_ACTION_DEVICEINSTANCEREMOVED;
    const bool removed = Action == CM_NOTIFY_ACTION_DEVICEINSTANCEREMOVED || Action == CM_NOTIFY_ACTION_DEVICEINTERFACEREMOVAL;
    if (!instance && Action != CM_NOTIFY_ACTION_DEVICEINTERFACEARRIVAL && !removed)
    {
        return ERROR_SUCCESS;
    }

    const auto* identifier = instance ? Data->u.DeviceInstance.InstanceId : Data->u.DeviceInterface.SymbolicLink;
    try
    {
        // Ignore non-USB events from the all-instance subscription.
        if (instance && !wsl::shared::string::StartsWith(identifier, L"USB\\", true))
        {
            return ERROR_SUCCESS;
        }

        // Wake readiness waiters and defer removal cleanup outside this callback.
        SetEvent(manager.m_changed.get());
        if (removed)
        {
            manager.QueueDeviceRemoval(identifier, instance);
        }
    }
    catch (...)
    {
        // A dropped notification must not disturb other devices.
    }
    return ERROR_SUCCESS;
}

void Manager::RunNotificationWorker() noexcept
try
{
    auto initialize = wil::CoInitializeEx(COINIT_MULTITHREADED);

    const HANDLE events[] = {m_stop.get(), m_work.get()};
    for (;;)
    {
        const auto wait = WaitForMultipleObjects(ARRAYSIZE(events), events, FALSE, INFINITE);
        THROW_LAST_ERROR_IF(wait == WAIT_FAILED);
        if (wait == WAIT_OBJECT_0)
        {
            return;
        }

        // Drop a failed batch without stopping subsequent notification processing.
        try
        {
            auto lock = m_lock.lock_exclusive();
            ProcessDeviceRemovals();
        }
        CATCH_LOG()
    }
}
CATCH_LOG()

void Manager::Stop() noexcept
try
{
    if (m_stopping.exchange(true))
    {
        return;
    }

    // Cancel readiness waits before waiting for callbacks and the worker to finish.
    m_stop.SetEvent();
    m_interfaceNotification.reset();
    m_instanceNotification.reset();

    if (m_worker.joinable())
    {
        m_worker.join();
    }

    // Release remaining reservations before service teardown completes.
    auto lock = m_lock.lock_exclusive();
    m_vms.clear();
    for (auto record = m_records.begin(); record != m_records.end();)
    {
        try
        {
            ReleaseReservation(*record);
            record = m_records.erase(record);
        }
        catch (...)
        {
            LOG_CAUGHT_EXCEPTION();
            ++record;
        }
    }
}
CATCH_LOG()

} // namespace wsl::windows::common::usb
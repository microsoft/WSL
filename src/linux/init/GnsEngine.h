// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include <functional>
#include <optional>
#include <tuple>
#include "util.h"
#include "lxinitshared.h"
#include "NetworkManager.h"
#include "DnsTunnelingManager.h"
#include "hns_schema.h"

class GnsEngine
{
public:
    struct Message
    {
        LX_MESSAGE_TYPE MessageType;
        std::string Json;
        std::optional<GUID> AdapterId;
    };

    using NotificationRoutine = std::function<std::optional<Message>()>;
    using StatusRoutine = std::function<void(int, const std::string&)>;

    GnsEngine(
        const NotificationRoutine& notificationRoutine,
        const StatusRoutine& statusRoutine,
        NetworkManager& manager,
        std::optional<int> dnsTunnelingFd,
        const std::string& dnsTunnelingIpAddress);

    void setup();

    void run();

private:
    std::tuple<bool, int> ProcessNextMessage();

    void ProcessNotification(const nlohmann::json& payload, Interface& interface);

    void ProcessRouteChange(Interface& interface, const wsl::shared::hns::Route& route, wsl::shared::hns::ModifyRequestType type);

    void ProcessIpAddressChange(Interface& interface, const wsl::shared::hns::IPAddress& route, wsl::shared::hns::ModifyRequestType type);

    void ProcessMacAddressChange(Interface& interface, const wsl::shared::hns::MacAddress& mac, wsl::shared::hns::ModifyRequestType type);

    void ProcessDNSChange(Interface& interface, const wsl::shared::hns::DNS& dns, wsl::shared::hns::ModifyRequestType type);

    void ProcessLinkChange(Interface& interface, const wsl::shared::hns::NetworkInterface& link, wsl::shared::hns::ModifyRequestType type);

    static Interface OpenAdapter(const GUID& id);

    static Interface OpenAdapterImpl(const GUID& id);

    static Interface OpenInterface(const std::string& deviceName);

    static Interface OpenInterfaceImpl(const std::string& deviceName);

    static Interface OpenInterfaceOrAdapter(const std::wstring& nameOrId);

    static std::optional<GUID> GetAdapterId(const std::string& name);

    template <typename T>
    static T Deserialize(const std::string& json);

    template <typename T>
    void ProcessNotificationImpl(
        Interface& interface, const nlohmann::json& payload, void (GnsEngine::*routine)(Interface&, const T&, wsl::shared::hns::ModifyRequestType));

    const NotificationRoutine& notificationRoutine;
    const StatusRoutine& statusRoutine;
    NetworkManager& manager;

    std::optional<DnsTunnelingManager> dnsTunnelingManager;
};

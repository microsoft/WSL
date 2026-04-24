/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    JsonUtils.h

Abstract:

    This file contains various JSON helper methods.

--*/

#pragma once

#include <nlohmann/json.hpp>
#include "stringshared.h"

#ifdef WIN32
#include "wslservice.h"
#include "ExecutionContext.h"
#include "wslc.h"
#endif

#define NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT_FROM_ONLY(Type, ...) \
    inline void from_json(const nlohmann::json& nlohmann_json_j, Type& nlohmann_json_t) \
    { \
        const Type nlohmann_json_default_obj{}; \
        NLOHMANN_JSON_EXPAND(NLOHMANN_JSON_PASTE(NLOHMANN_JSON_FROM_WITH_DEFAULT, __VA_ARGS__)) \
    }

namespace wsl::shared {

constexpr int c_jsonPrettyPrintIndent = 2;

template <typename T>
std::string ToJson(const T& Value, int indent = -1)
{
    nlohmann::json json;
    to_json(json, Value);

    return json.dump(indent);
}

template <typename T>
std::wstring ToJsonW(const T& Value, int indent = -1)
{
    return wsl::shared::string::MultiByteToWide(ToJson(Value, indent));
}

template <typename T, typename TJson = nlohmann::json>
T FromJson(const char* Value)
{
    try
    {
        auto json = TJson::parse(Value);
        T object{};
        from_json(json, object);

        return object;
    }
    catch (const TJson::exception& e)
    {

#ifdef WIN32

        THROW_HR_WITH_USER_ERROR_MSG(
            WSL_E_INVALID_JSON, wsl::shared::Localization::MessageInvalidJson(e.what()), "Invalid JSON: %hs", Value);

#else
        LOG_ERROR("Failed to deserialize json: '{}'. Error: {}", Value, e.what());
        THROW_ERRNO(EINVAL);

#endif
    }
}

template <typename T, typename TJson = nlohmann::json>
T FromJson(const wchar_t* Value)
{
    return FromJson<T, TJson>(wsl::shared::string::WideToMultiByte(Value).c_str());
}

template <typename T>
std::string JsonEnumToString(T value)
{
    nlohmann::json json;
    to_json(json, value);

    return json.get<std::string>();
}
} // namespace wsl::shared

namespace nlohmann {

template <>
struct adl_serializer<std::wstring>
{
    static void to_json(json& j, const std::wstring& str)
    {
        j = wsl::shared::string::WideToMultiByte(str);
    }

    static void from_json(const json& j, std::wstring& str)
    {
        str = wsl::shared::string::MultiByteToWide(j.get<std::string>());
    }
};

template <typename T>
struct adl_serializer<std::optional<T>>
{
    static void to_json(json& j, const std::optional<T>& input)
    {
        if (input.has_value())
        {
            j = input.value();
        }
        else
        {
            j = nullptr;
        }
    }

    static void from_json(const json& j, std::optional<T>& input)
    {
        if (!j.is_null())
        {
            input.emplace(); // Assumes that object is default constructible.
            adl_serializer<T>::from_json(j, input.value());
        }
    }
};

template <>
struct adl_serializer<GUID>
{
    static void to_json(json& j, const GUID& input)
    {
        j = wsl::shared::string::GuidToString<char>(input, wsl::shared::string::GuidToStringFlags::None);
    }

    static void from_json(const json& j, GUID& output)
    {
        const auto parsed = wsl::shared::string::ToGuid(j.get<std::string>());
        if (parsed.has_value())
        {
            output = parsed.value();
        }
    }
};

template <>
struct adl_serializer<wsl::shared::string::MacAddress>
{
    static void to_json(json& j, const wsl::shared::string::MacAddress& input)
    {
        j = wsl::shared::string::FormatMacAddress(input, '-');
    }

    static void from_json(const json& j, wsl::shared::string::MacAddress& output)
    {
        if (j.is_string())
        {
            auto parsed = wsl::shared::string::ParseMacAddressNoThrow(j.get<std::string>());
            if (parsed.has_value())
            {
                output = std::move(parsed.value());
            }
        }
    }
};

#ifdef WIN32
template <>
struct adl_serializer<WSLCVolumeInformation>
{
    static void to_json(json& j, const WSLCVolumeInformation& volume)
    {
        j = json{{"Name", std::string(volume.Name)}, {"Driver", std::string(volume.Driver)}};
    }

    static void from_json(const json& j, WSLCVolumeInformation& volume)
    {
        std::string name = j.at("Name").get<std::string>();
        std::string driver = j.at("Driver").get<std::string>();

        strncpy_s(volume.Name, sizeof(volume.Name), name.c_str(), _TRUNCATE);
        strncpy_s(volume.Driver, sizeof(volume.Driver), driver.c_str(), _TRUNCATE);
    }
};

template <>
struct adl_serializer<WSLCNetworkInformation>
{
    static void to_json(json& j, const WSLCNetworkInformation& network)
    {
        j = json{{"Name", std::string(network.Name)}, {"Id", std::string(network.Id)}, {"Driver", std::string(network.Driver)}};
    }

    static void from_json(const json& j, WSLCNetworkInformation& network)
    {
        std::string name = j.at("Name").get<std::string>();
        std::string id = j.at("Id").get<std::string>();
        std::string driver = j.at("Driver").get<std::string>();

        strncpy_s(network.Name, sizeof(network.Name), name.c_str(), _TRUNCATE);
        strncpy_s(network.Id, sizeof(network.Id), id.c_str(), _TRUNCATE);
        strncpy_s(network.Driver, sizeof(network.Driver), driver.c_str(), _TRUNCATE);
    }
};
#endif

} // namespace nlohmann
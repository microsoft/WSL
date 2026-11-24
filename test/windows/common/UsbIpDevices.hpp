// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace wsl::test::usbip {

enum class Device
{
    Serial,
    Audio
};

inline constexpr uint16_t c_vendorId = 0x1209;

using Interface = std::array<uint8_t, 4>;
inline constexpr std::array c_serialInterfaces{Interface{0x02, 0x02, 0, 0}, Interface{0x0a, 0, 0, 0}};
inline constexpr std::array<Interface, 1> c_audioInterfaces{Interface{0x01, 0x01, 0, 0}};

template <size_t Size>
consteval auto Configuration(uint8_t interfaces, const std::array<uint8_t, Size>& descriptors)
{
    constexpr size_t length = 9 + Size;
    std::array<uint8_t, length> result{9, 2, static_cast<uint8_t>(length), static_cast<uint8_t>(length >> 8), interfaces, 1, 0, 0x80, 50};
    std::copy(descriptors.begin(), descriptors.end(), result.begin() + 9);
    return result;
}

inline constexpr auto c_serialConfiguration = Configuration(
    2, std::to_array<uint8_t>({9,    4, 0,    0, 1,    0x02, 0x02, 0, 0,  5, 0x24, 0, 0x10, 0x01, 5,  0x24, 1, 0, 1, 4,
                               0x24, 2, 0x02, 5, 0x24, 6,    0,    1, 7,  5, 0x81, 3, 16,   0,    16, 9,    4, 1, 0, 2,
                               0x0a, 0, 0,    0, 7,    5,    0x02, 2, 64, 0, 0,    7, 5,    0x82, 2,  64,   0, 0}));

inline constexpr auto c_audioConfiguration =
    Configuration(1, std::to_array<uint8_t>({9, 4, 0, 0, 0, 0x01, 0x01, 0, 0, 8, 0x24, 1,    0, 1, 29, 0, 0, 12, 0x24,
                                             2, 1, 1, 2, 0, 1,    0,    0, 0, 0, 9,    0x24, 3, 2, 1,  3, 0, 1,  0}));

struct Profile
{
    std::string_view BusId;
    std::string_view Name;
    uint16_t ProductId;
    uint8_t DeviceClass;
    uint8_t DeviceSubclass;
    std::span<const Interface> Interfaces;
    std::span<const uint8_t> Config;
};

inline constexpr Profile c_serial{"1-1", "WSL USB Loopback", 1, 2, 2, c_serialInterfaces, c_serialConfiguration};
inline constexpr Profile c_audio{"1-2", "WSL USB Audio", 2, 0, 0, c_audioInterfaces, c_audioConfiguration};

constexpr const Profile& GetProfile(Device device)
{
    return device == Device::Serial ? c_serial : c_audio;
}

} // namespace wsl::test::usbip
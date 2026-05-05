// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include <array>
#include <random>
#include <string>

namespace wsl::windows::service::wslc {

// 55 descriptors x 56 mountains = 3,080 unique combinations.
// With retry digit suffix (0-9), up to 30,800 unique names.

constexpr std::array c_descriptors = {
    "swift",       "bold",      "misty",       "golden",     "rugged",     "serene",    "mighty",
    "noble",       "silent",    "ancient",     "bright",     "calm",       "crisp",     "daring",
    "eager",       "fierce",    "gentle",      "hidden",     "icy",        "jade",      "keen",
    "lofty",       "majestic",  "nimble",      "proud",      "quiet",      "radiant",   "snowy",
    "stellar",     "tranquil",  "untamed",     "vast",       "wandering",  "wild",      "azure",
    "blazing",     "cloudy",    "dusty",       "emerald",    "frosty",     "gleaming",  "hazy",
    "ivory",       "jovial",    "luminous",    "mossy",      "northern",   "onyx",      "peaceful",
    "rustic",      "shimmering","twilight",    "verdant",    "whispering", "zephyr",
};

constexpr std::array c_mountains = {
    // Asia
    "himalaya",  "karakoram",  "hindukush", "pamirs",    "tienshan",  "kunlun",       "altai",
    "zagros",    "elburz",     "caucasus",  "annamite",
    // Europe
    "alps",      "pyrenees",   "carpathian","apennine",  "balkan",    "dinaric",      "scandinavian",
    "dolomites", "urals",
    // North America
    "rockies",   "appalachian","sierra",    "cascade",   "olympic",   "brooks",       "alaska",
    "ozark",     "adirondack", "catskill",  "bighorn",   "bitterroot","sawtooth",     "teton",
    "wasatch",   "sangre",     "uinta",     "absaroka",  "beartooth", "laramie",      "medicine",
    // South America
    "andes",     "cordillera",
    // Africa
    "atlas",     "drakensberg","rwenzori",  "simien",    "virunga",   "tibesti",      "ahaggar",
    // Oceania
    "dividing",  "macdonnell", "flinders",  "stirling",
    // Antarctica
    "transantarctic", "ellsworth",
};

// Generate a random container name in the format "descriptor_mountain".
// When retry > 0, appends a random digit (0-9) to reduce collisions.
inline std::string GenerateContainerName(int retry)
{
    std::random_device rd;
    std::mt19937 gen(rd());

    std::uniform_int_distribution<size_t> leftDist(0, c_descriptors.size() - 1);
    std::uniform_int_distribution<size_t> rightDist(0, c_mountains.size() - 1);

    auto name = std::string(c_descriptors[leftDist(gen)]) + "_" + c_mountains[rightDist(gen)];

    if (retry > 0)
    {
        std::uniform_int_distribution<int> digitDist(0, 9);
        name += std::to_string(digitDist(gen));
    }

    return name;
}

// Generate a random 12-character hex string as a fallback name.
// Mirrors Docker's fallback to truncated container ID when all human-readable retries are exhausted.
inline std::string GenerateRandomHexName()
{
    std::random_device rd;
    std::independent_bits_engine<std::default_random_engine, CHAR_BIT, unsigned short> random(rd());

    std::array<unsigned short, 6> randomBytes;
    std::generate(randomBytes.begin(), randomBytes.end(), random);

    std::string name;
    name.reserve(randomBytes.size() * 2);
    for (auto b : randomBytes)
    {
        std::format_to(std::back_inserter(name), "{:02x}", static_cast<BYTE>(b));
    }

    return name;
}

} // namespace wsl::windows::service::wslc

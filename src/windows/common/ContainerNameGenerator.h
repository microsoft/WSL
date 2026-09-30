/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ContainerNameGenerator.h

Abstract:

    Constants for auto generated container names.

--*/

#pragma once

#include <array>

namespace wsl::windows::service::wslc {

constexpr std::array c_descriptors = {
    "swift",      "bold",     "misty",     "golden",     "rugged",   "serene",  "mighty",   "noble",  "silent",   "ancient",
    "bright",     "calm",     "crisp",     "daring",     "eager",    "fierce",  "gentle",   "hidden", "icy",      "jade",
    "keen",       "lofty",    "majestic",  "nimble",     "proud",    "quiet",   "radiant",  "snowy",  "stellar",  "tranquil",
    "untamed",    "vast",     "wandering", "wild",       "azure",    "blazing", "cloudy",   "dusty",  "emerald",  "frosty",
    "gleaming",   "hazy",     "ivory",     "jovial",     "luminous", "mossy",   "northern", "onyx",   "peaceful", "rustic",
    "shimmering", "twilight", "verdant",   "whispering", "zephyr",
};

constexpr std::array c_mountains = {
    // Asia
    "himalaya",
    "karakoram",
    "hindukush",
    "pamirs",
    "tienshan",
    "kunlun",
    "altai",
    "zagros",
    "elburz",
    "caucasus",
    "annamite",
    // Europe
    "alps",
    "pyrenees",
    "carpathian",
    "apennine",
    "balkan",
    "dinaric",
    "scandinavian",
    "dolomites",
    "urals",
    // North America
    "rockies",
    "appalachian",
    "sierra",
    "cascade",
    "olympic",
    "brooks",
    "alaska",
    "ozark",
    "adirondack",
    "catskill",
    "bighorn",
    "bitterroot",
    "sawtooth",
    "teton",
    "wasatch",
    "sangre",
    "uinta",
    "absaroka",
    "beartooth",
    "laramie",
    "medicine",
    // South America
    "andes",
    "cordillera",
    // Africa
    "atlas",
    "drakensberg",
    "rwenzori",
    "simien",
    "virunga",
    "tibesti",
    "ahaggar",
    // Oceania
    "dividing",
    "macdonnell",
    "flinders",
    "stirling",
    // Antarctica
    "transantarctic",
    "ellsworth",
};

} // namespace wsl::windows::service::wslc

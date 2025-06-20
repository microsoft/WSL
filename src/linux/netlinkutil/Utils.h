// Copyright (C) Microsoft Corporation. All rights reserved.
#pragma once

#include <ostream>
#include <vector>
#include <linux/rtnetlink.h>
#include <linux/fib_rules.h>
#include "address.h"

// TODO: Maybe use a template and __function__ to write the type name compile
// time ?

namespace utils {
template <typename T>
struct Attribute
{
    rtattr header __attribute__((aligned(RTA_ALIGNTO)));
    T value __attribute__((aligned(RTA_ALIGNTO)));
} __attribute__((packed));

template <bool Empty, typename T>
struct Optional;

template <typename T>
struct Optional<false, T>
{
    /// empty, though note that empty structs are of size 1 byte.
};

template <typename T>
struct Optional<true, T>
{
    T attribute;
};

template <typename TAddr>
struct AddressAttribute
{
    rtattr header;
    TAddr address;
} __attribute__((packed));

struct CacheInfoAttribute
{
    rtattr header;
    struct ifa_cacheinfo cacheinfo;
} __attribute((packed));

struct MacAddressAttribute
{
    nlattr header;
    MacAddress address;
} __attribute__((packed));

struct IntegerAttribute
{
    rtattr header;
    int value;
} __attribute__((packed));

std::ostream& FormatBinary(std::ostream& out, const void* ptr, size_t bytes);

template <typename T>
std::ostream& FormatArray(std::ostream& out, const std::vector<T>& content);

std::string BytesToHex(const void* ptr, size_t bytes, const std::string& separator);

template <typename TAddr>
void InitializeAddressAttribute(AddressAttribute<TAddr>& attribute, const Address& address, int type);

void InitializeCacheInfoAttribute(CacheInfoAttribute& attribute, const Address& address);

void InitializeIntegerAttribute(IntegerAttribute& attribute, int value, int type);

Address ComputeBroadcastAddress(const Address& address);

template <typename T>
std::string Stringify(const T& value);
} // namespace utils

#include "Utils.hxx"

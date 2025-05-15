// Copyright (C) Microsoft Corporation. All rights reserved.
#pragma once

// Lxcore type definitions.
typedef int LX_INT;
typedef unsigned long LX_UID_T;
typedef unsigned long LX_GID_T;
typedef unsigned long LX_MODE_T;

// Windows-like type definitions
typedef std::uint8_t UINT8;
typedef std::int8_t INT8;
typedef std::uint16_t UINT16;
typedef std::int16_t INT16;
typedef std::uint32_t UINT32;
typedef std::int32_t INT32;
typedef std::uint64_t UINT64;
typedef std::int64_t INT64;
typedef std::uintptr_t ULONG_PTR;
typedef unsigned char UCHAR;
typedef const char* PCSTR;

typedef struct _GUID
{
    std::uint32_t Data1;
    std::uint16_t Data2;
    std::uint16_t Data3;
    std::uint8_t Data4[8];

    bool operator==(const _GUID& other) const
    {
        return memcmp(this, &other, sizeof(*this)) == 0;
    }

    bool operator!=(const _GUID& other) const
    {
        return !(*this == other);
    }
} GUID;

static_assert(sizeof(GUID) == 16);

// Lxcore versions of error values.
// N.B. These are negative.
#define LX_EPERM (-EPERM)
#define LX_ENOENT (-ENOENT)
#define LX_ESRCH (-ESRCH)
#define LX_EINTR (-EINTR)
#define LX_EIO (-EIO)
#define LX_ENXIO (-ENXIO)
#define LX_ENOEXEC (-ENOEXEC)
#define LX_E2BIG (-E2BIG)
#define LX_EBADF (-EBADF)
#define LX_ECHILD (-ECHILD)
#define LX_EAGAIN (-EAGAIN)
#define LX_EWOULDBLOCK (-EWOULDBLOCK)
#define LX_ENOMEM (-ENOMEM)
#define LX_EACCES (-EACCES)
#define LX_EFAULT (-EFAULT)
#define LX_EBUSY (-EBUSY)
#define LX_EEXIST (-EEXIST)
#define LX_EXDEV (-EXDEV)
#define LX_ENODEV (-ENODEV)
#define LX_ENOTDIR (-ENOTDIR)
#define LX_EISDIR (-EISDIR)
#define LX_EINVAL (-EINVAL)
#define LX_ENFILE (-ENFILE)
#define LX_EMFILE (-EMFILE)
#define LX_ENOTTY (-ENOTTY)
#define LX_EFBIG (-EFBIG)
#define LX_ENOSPC (-ENOSPC)
#define LX_ESPIPE (-ESPIPE)
#define LX_EROFS (-EROFS)
#define LX_EMLINK (-EMLINK)
#define LX_EPIPE (-EPIPE)
#define LX_ERANGE (-ERANGE)
#define LX_EDEADLK (-EDEADLK)
#define LX_ENAMETOOLONG (-ENAMETOOLONG)
#define LX_ENOLCK (-ENOLCK)
#define LX_ENOSYS (-ENOSYS)
#define LX_ENOTEMPTY (-ENOTEMPTY)
#define LX_ELOOP (-ELOOP)
#define LX_EIDRM (-EIDRM)
#define LX_ENODATA (-ENODATA)
#define LX_ENOATTR (-ENOATTR)
#define LX_EPROTO (-EPROTO)
#define LX_EOVERFLOW (-EOVERFLOW)
#define LX_EUSERS (-EUSERS)
#define LX_ENOTSOCK (-ENOTSOCK)
#define LX_EDESTADDRREQ (-EDESTADDRREQ)
#define LX_EMSGSIZE (-EMSGSIZE)
#define LX_EPROTOTYPE (-EPROTOTYPE)
#define LX_ENOPROTOOPT (-ENOPROTOOPT)
#define LX_EPROTONOSUPPORT (-EPROTONOSUPPORT)
#define LX_ESOCKTNOSUPPORT (-ESOCKTNOSUPPORT)
#define LX_EOPNOTSUPP (-EOPNOTSUPP)
#define LX_ENOTSUP (-ENOTSUP)
#define LX_EAFNOSUPPORT (-EAFNOSUPPORT)
#define LX_EADDRINUSE (-EADDRINUSE)
#define LX_EADDRNOTAVAIL (-EADDRNOTAVAIL)
#define LX_ENETUNREACH (-ENETUNREACH)
#define LX_ECONNABORTED (-ECONNABORTED)
#define LX_ECONNRESET (-ECONNRESET)
#define LX_ENOBUFS (-ENOBUFS)
#define LX_EISCONN (-EISCONN)
#define LX_ENOTCONN (-ENOTCONN)
#define LX_ETIMEDOUT (-ETIMEDOUT)
#define LX_ECONNREFUSED (-ECONNREFUSED)
#define LX_EHOSTDOWN (-EHOSTDOWN)
#define LX_EHOSTUNREACH (-EHOSTUNREACH)
#define LX_EALREADY (-EALREADY)
#define LX_EINPROGRESS (-EINPROGRESS)
#define LX_ENOMEDIUM (-ENOMEDIUM)
#define LX_EMEDIUMTYPE (-EMEDIUMTYPE)
#define LX_ECANCELED (-ECANCELED)
#define LX_ENOKEY (-ENOKEY)

// Lxcore version of constants
#define LX_PATH_MAX (PATH_MAX)
#define LX_DT_DIR (DT_DIR)
#define LX_DT_LNK (DT_LNK)

// Other definitions available on NT.
extern "C++" {

template <size_t S>
struct _ENUM_FLAG_INTEGER_FOR_SIZE;

template <>
struct _ENUM_FLAG_INTEGER_FOR_SIZE<1>
{
    typedef INT8 type;
};

template <>
struct _ENUM_FLAG_INTEGER_FOR_SIZE<2>
{
    typedef INT16 type;
};

template <>
struct _ENUM_FLAG_INTEGER_FOR_SIZE<4>
{
    typedef INT32 type;
};

template <>
struct _ENUM_FLAG_INTEGER_FOR_SIZE<8>
{
    typedef INT64 type;
};

// used as an approximation of std::underlying_type<T>
template <class T>
struct _ENUM_FLAG_SIZED_INTEGER
{
    typedef typename _ENUM_FLAG_INTEGER_FOR_SIZE<sizeof(T)>::type type;
};
}

#define _ENUM_FLAG_CONSTEXPR constexpr
#define DEFINE_ENUM_FLAG_OPERATORS(ENUMTYPE) \
    extern "C++" { \
    inline _ENUM_FLAG_CONSTEXPR ENUMTYPE operator|(ENUMTYPE a, ENUMTYPE b) throw() \
    { \
        return ENUMTYPE(((_ENUM_FLAG_SIZED_INTEGER<ENUMTYPE>::type)a) | ((_ENUM_FLAG_SIZED_INTEGER<ENUMTYPE>::type)b)); \
    } \
    inline ENUMTYPE& operator|=(ENUMTYPE& a, ENUMTYPE b) throw() \
    { \
        return (ENUMTYPE&)(((_ENUM_FLAG_SIZED_INTEGER<ENUMTYPE>::type&)a) |= ((_ENUM_FLAG_SIZED_INTEGER<ENUMTYPE>::type)b)); \
    } \
    inline _ENUM_FLAG_CONSTEXPR ENUMTYPE operator&(ENUMTYPE a, ENUMTYPE b) throw() \
    { \
        return ENUMTYPE(((_ENUM_FLAG_SIZED_INTEGER<ENUMTYPE>::type)a) & ((_ENUM_FLAG_SIZED_INTEGER<ENUMTYPE>::type)b)); \
    } \
    inline ENUMTYPE& operator&=(ENUMTYPE& a, ENUMTYPE b) throw() \
    { \
        return (ENUMTYPE&)(((_ENUM_FLAG_SIZED_INTEGER<ENUMTYPE>::type&)a) &= ((_ENUM_FLAG_SIZED_INTEGER<ENUMTYPE>::type)b)); \
    } \
    inline _ENUM_FLAG_CONSTEXPR ENUMTYPE operator~(ENUMTYPE a) throw() \
    { \
        return ENUMTYPE(~((_ENUM_FLAG_SIZED_INTEGER<ENUMTYPE>::type)a)); \
    } \
    inline _ENUM_FLAG_CONSTEXPR ENUMTYPE operator^(ENUMTYPE a, ENUMTYPE b) throw() \
    { \
        return ENUMTYPE(((_ENUM_FLAG_SIZED_INTEGER<ENUMTYPE>::type)a) ^ ((_ENUM_FLAG_SIZED_INTEGER<ENUMTYPE>::type)b)); \
    } \
    inline ENUMTYPE& operator^=(ENUMTYPE& a, ENUMTYPE b) throw() \
    { \
        return (ENUMTYPE&)(((_ENUM_FLAG_SIZED_INTEGER<ENUMTYPE>::type&)a) ^= ((_ENUM_FLAG_SIZED_INTEGER<ENUMTYPE>::type)b)); \
    } \
    }

typedef struct _LIST_ENTRY
{
    struct _LIST_ENTRY* Flink;
    struct _LIST_ENTRY* Blink;
} LIST_ENTRY, *PLIST_ENTRY;

inline void InitializeListHead(PLIST_ENTRY listHead)
{
    listHead->Flink = listHead->Blink = listHead;
}

[[nodiscard]] inline bool IsListEmpty(const LIST_ENTRY* listHead)
{
    return listHead->Flink == listHead;
}

inline void InsertTailList(PLIST_ENTRY listHead, PLIST_ENTRY entry)
{
    PLIST_ENTRY blink = listHead->Blink;
    entry->Flink = listHead;
    entry->Blink = blink;
    blink->Flink = entry;
    listHead->Blink = entry;
}

inline bool RemoveEntryList(PLIST_ENTRY entry)
{
    PLIST_ENTRY flink = entry->Flink;
    PLIST_ENTRY blink = entry->Blink;
    blink->Flink = flink;
    flink->Blink = blink;
    return flink == blink;
}

#define CONTAINING_RECORD(address, type, field) ((type*)((char*)(address) - (ULONG_PTR)(&((type*)0)->field)))

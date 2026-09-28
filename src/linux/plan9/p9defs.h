// Copyright (C) Microsoft Corporation. All rights reserved.
#pragma once

namespace p9fs {

constexpr std::string_view ProtocolVersionL{"9P2000.L"};
constexpr std::string_view ProtocolVersionW{"9P2000.W"};

// The plan9 message type. Messages starting with T are request messages, and
// messages starting with R are response messages. The response message is always
// one plus the request message.
enum class MessageType : UINT8
{
    Tlerror = 6,
    Rlerror,
    Tstatfs = 8,
    Rstatfs,
    Tlopen = 12,
    Rlopen,
    Tlcreate = 14,
    Rlcreate,
    Tsymlink = 16,
    Rsymlink,
    Tmknod = 18,
    Rmknod,
    Trename = 20,
    Rrename,
    Treadlink = 22,
    Rreadlink,
    Tgetattr = 24,
    Rgetattr,
    Tsetattr = 26,
    Rsetattr,
    Txattrwalk = 30,
    Rxattrwalk,
    Txattrcreate = 32,
    Rxattrcreate,
    Treaddir = 40,
    Rreaddir,
    Tfsync = 50,
    Rfsync,
    Tlock = 52,
    Rlock,
    Tgetlock = 54,
    Rgetlock,
    Tlink = 70,
    Rlink,
    Tmkdir = 72,
    Rmkdir,
    Trenameat = 74,
    Rrenameat,
    Tunlinkat = 76,
    Runlinkat,
    Tversion = 100,
    Rversion,
    Tauth = 102,
    Rauth,
    Tattach = 104,
    Rattach,
    Terror = 106,
    Rerror,
    Tflush = 108,
    Rflush,
    Twalk = 110,
    Rwalk,
    Topen = 112,
    Ropen,
    Tcreate = 114,
    Rcreate,
    Tread = 116,
    Rread,
    Twrite = 118,
    Rwrite,
    Tclunk = 120,
    Rclunk,
    Tremove = 122,
    Rremove,
    Tstat = 124,
    Rstat,
    Twstat = 126,
    Rwstat,
    // 9P2000.W messages:
    // N.B. 9P2000.W is a currently unofficial extension to 9P2000.L which includes some messages
    //      used by the Windows Plan 9 redirector for improved functionality and performance.
    Taccess = 128,
    Raccess,
    Twreaddir = 130,
    Rwreaddir,
    Twopen = 132,
    Rwopen
};

// The type of the file, as indicated in a Qid.
enum class QidType : UINT8
{
    File = 0x00,
    Link = 0x01,
    Symlink = 0x02,
    Temp = 0x04,
    Auth = 0x08,
    MountPoint = 0x10,
    Exclusive = 0x20,
    Append = 0x40,
    Directory = 0x80
};

DEFINE_ENUM_FLAG_OPERATORS(QidType);

// A type representing a unique file identifier. On Linux, the path is used as
// the inode number.
struct Qid
{
    UINT64 Path;
    UINT32 Version;
    QidType Type;
};

// File system attributes, used in statfs
struct StatFsResult
{
    UINT32 Type;
    UINT32 BlockSize;
    UINT64 Blocks;
    UINT64 BlocksFree;
    UINT64 BlocksAvailable;
    UINT64 Files;
    UINT64 FilesFree;
    UINT64 FsId;
    UINT32 NameLength;
};

// Linux file attributes, used in getattr and setattr.
struct StatResult
{
    UINT32 Mode;
    UINT32 Uid;
    UINT32 Gid;
    UINT64 NLink;
    UINT64 RDev;
    UINT64 Size;
    UINT64 BlockSize;
    UINT64 Blocks;
    UINT64 AtimeSec;
    UINT64 AtimeNsec;
    UINT64 MtimeSec;
    UINT64 MtimeNsec;
    UINT64 CtimeSec;
    UINT64 CtimeNsec;
};

// Wire size of the StatResult structure.
// N.B. Can't use sizeof(StatResult) because it contains padding.
constexpr UINT32 StatResultSize = (3 * sizeof(UINT32)) + (11 * sizeof(UINT64));

constexpr UINT32 TagOffset = sizeof(UINT32) + sizeof(UINT8);
constexpr UINT32 HeaderSize = sizeof(UINT32) + sizeof(UINT8) + sizeof(UINT16);
constexpr UINT32 QidSize = sizeof(UINT8) + sizeof(UINT32) + sizeof(UINT64);
constexpr UINT32 NoFid = ~0u;

// The "diod" server requires the max IO buffer to be at least 24 bytes smaller than the negotiated
// message size. While our Plan 9 server does not require it, it's used just in case.
constexpr UINT32 IoHeaderSize = 24;

// High bits for file modes. The low bits are used
// for permission bits.
constexpr UINT32 ModeDirectory = 040000;
constexpr UINT32 ModeRegularFile = 0100000;

// Bitmask of valid attributes for getattr requests.
constexpr UINT32 GetAttrMode = 0x1;
constexpr UINT32 GetAttrNlink = 0x2;
constexpr UINT32 GetAttrUid = 0x4;
constexpr UINT32 GetAttrGid = 0x8;
constexpr UINT32 GetAttrRdev = 0x10;
constexpr UINT32 GetAttrAtime = 0x20;
constexpr UINT32 GetAttrMtime = 0x40;
constexpr UINT32 GetAttrCtime = 0x80;
constexpr UINT32 GetAttrIno = 0x100;
constexpr UINT32 GetAttrSize = 0x200;
constexpr UINT32 GetAttrBlocks = 0x400;
constexpr UINT32 GetAttrBtime = 0x800;
constexpr UINT32 GetAttrGen = 0x1000;
constexpr UINT32 GetAttrDataVersion = 0x2000;

// Bitmask of valid attributes for setattr requests.
constexpr UINT32 SetAttrMode = 0x1;
constexpr UINT32 SetAttrUid = 0x2;
constexpr UINT32 SetAttrGid = 0x4;
constexpr UINT32 SetAttrSize = 0x8;
constexpr UINT32 SetAttrAtime = 0x10;
constexpr UINT32 SetAttrMtime = 0x20;
constexpr UINT32 SetAttrCtime = 0x40;
constexpr UINT32 SetAttrAtimeSet = 0x80;
constexpr UINT32 SetAttrMtimeSet = 0x100;

// Flags for the lopen and create messages.
// N.B. These may not be identical to Linux open flags on all platforms.
enum class OpenFlags
{
    ReadOnly = 0,
    WriteOnly = 01,
    ReadWrite = 02,
    NoAccess = 03,
    AccessMask = 03,
    Create = 0100,
    Exclusive = 0200,
    NoCTTY = 0400,
    Truncate = 01000,
    Append = 02000,
    NonBlock = 04000,
    DSync = 010000,
    FAsync = 00020000,
    Direct = 00040000,
    LargeFile = 00100000,
    Directory = 0200000,
    NoFollow = 00400000,
    NoAccessTime = 01000000,
    CloseOnExec = 02000000,
    Sync = 04000000
};

DEFINE_ENUM_FLAG_OPERATORS(OpenFlags);

// File lock types
enum class LockType
{
    ReadLock,
    WriteLock,
    Unlock
};

// Status values returned by the lock message.
enum class LockStatus
{
    Success,
    Blocked,
    Error,
    Grace
};

enum class AccessFlags
{
    Ok = 0,
    Execute = 1,
    Write = 2,
    Read = 4,
    Delete = 8,
    All = Execute | Write | Read | Delete
};

DEFINE_ENUM_FLAG_OPERATORS(AccessFlags);

// Flags for the wopen message.
enum class WOpenFlags
{
    None = 0,
    DeleteAccess = 0x1,
    NonDirectoryFile = 0x2,
    OpenSymlink = 0x4,
};

DEFINE_ENUM_FLAG_OPERATORS(WOpenFlags);

// Status values returned by the wopen message.
enum class WOpenStatus : UINT8
{
    Opened,
    Created,
    ParentNotFound,
    NotFound,
    Stopped,
};

} // namespace p9fs

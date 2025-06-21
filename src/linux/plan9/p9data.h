/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    p9data.h

Abstract:

    This file contains the plan9 message types to size mapping logic.

--*/

#pragma once

namespace p9fs {

// Gets the minimum message size for each Plan 9 message type.
// If the messages can be a dynamic length, this size is the minimum size of
// the message if all dynamic content is omitted. In the case of strings, the
// size of the two-byte length field is included, but the string length itself
// is not. The omitted components are listed with each message type, and the
// caller is responsible for adding the right values if necessary.
constexpr UINT32 GetMessageSize(MessageType messageType)
{
    switch (messageType)
    {
    case MessageType::Tversion:
        // size[4] Tversion tag[2] msize[4] version[s]
        // Excludes: version string data
        return HeaderSize + /*msize*/ 4 + /*version*/ 2;

    case MessageType::Rversion:
        // size[4] Rversion tag[2] msize[4] version[s]
        // Excludes: version string data
        return HeaderSize + /*msize*/ 4 + /*version*/ 2;

    case MessageType::Tflush:
        // size[4] Tflush tag[2] oldtag[2]
        return HeaderSize + /*oldtag*/ 2;

    case MessageType::Rflush:
        // size[4] Rflush tag[2]
        return HeaderSize;

    case MessageType::Twalk:
        // size[4] Twalk tag[2] fid[4] newfid[4] nwname[2] nwname*(wname[s])
        // Excludes: repeated elements
        return HeaderSize + /*fid*/ 4 + /*newfid*/ 4 + /*nwname*/ 2;

    case MessageType::Rwalk:
        // size[4] Rwalk tag[2] nwqid[2] nwqid*(wqid[13])
        // Excludes: repeated elements
        return HeaderSize + /*nwqid*/ 2;

    case MessageType::Tread:
        // size[4] Tread tag[2] fid[4] offset[8] count[4]
        return HeaderSize + /*fid*/ 4 + /*offset*/ 8 + /*count*/ 4;

    case MessageType::Rread:
        // size[4] Rread tag[2] count[4] data[count]
        // Excludes: data
        return HeaderSize + /*count*/ 4;

    case MessageType::Twrite:
        // size[4] Twrite tag[2] fid[4] offset[8] count[4] data[count]
        // Excludes: data
        return HeaderSize + /*fid*/ 4 + /*offset*/ 8 + /*count*/ 4;

    case MessageType::Rwrite:
        // size[4] Rwrite tag[2] count[4]
        return HeaderSize + /*count*/ 4;

    case MessageType::Tclunk:
        // size[4] Tclunk tag[2] fid[4]
        return HeaderSize + /*fid*/ 4;

    case MessageType::Rclunk:
        // size[4] Rclunk tag[2]
        return HeaderSize;

    case MessageType::Tremove:
        // size[4] Tremove tag[2] fid[4]
        return HeaderSize + /*fid*/ 4;

    case MessageType::Rremove:
        // size[4] Rremove tag[2]
        return HeaderSize;

    case MessageType::Tauth:
        // size[4] Tauth tag[2] afid[4] uname[s] aname[s] n_uname[4]
        // Excludes: uname string data, aname string data
        return HeaderSize + /*afid*/ 4 + /*uname*/ 2 + /*aname*/ 2 + /*n_uname*/ 4;

    case MessageType::Rauth:
        // size[4] Rauth tag[2] aqid[13]
        return HeaderSize + /*aqid*/ 13;

    case MessageType::Tattach:
        // size[4] Tattach tag[2] fid[4] afid[4] uname[s] aname[s] n_uname[4]
        // Excludes: uname string data, aname string data
        return HeaderSize + /*fid*/ 4 + /*afid*/ 4 + /*uname*/ 2 + /*aname*/ 2 + /*n_uname*/ 4;

    case MessageType::Rattach:
        // size[4] Rattach tag[2] qid[13]
        return HeaderSize + /*qid*/ 13;

    case MessageType::Rlerror:
        // size[4] Rlerror tag[2] ecode[4]
        return HeaderSize + /*ecode*/ 4;

    case MessageType::Tstatfs:
        // size[4] Tstatfs tag[2] fid[4]
        return HeaderSize + /*fid*/ 4;

    case MessageType::Rstatfs:
        // size[4] Rstatfs tag[2] type[4] bsize[4] blocks[8] bfree[8] bavail[8] files[8] ffree[8] fsid[8] namelen[4]
        return HeaderSize + /*type*/ 4 + /*bsize*/ 4 + /*blocks*/ 8 + /*bfree*/ 8 + /*bavail*/ 8 + /*files*/ 8 + /*ffree*/ 8 +
               /*fsid*/ 8 + /*namelen*/ 4;

    case MessageType::Tlopen:
        // size[4] Tlopen tag[2] fid[4] flags[4]
        return HeaderSize + /*fid*/ 4 + /*flags*/ 4;

    case MessageType::Rlopen:
        // size[4] Rlopen tag[2] qid[13] iounit[4]
        return HeaderSize + /*qid*/ 13 + /*iounit*/ 4;

    case MessageType::Tlcreate:
        // size[4] Tlcreate tag[2] fid[4] name[s] flags[4] mode[4] gid[4]
        // Excludes: name string data
        return HeaderSize + /*fid*/ 4 + /*name*/ 2 + /*flags*/ 4 + /*mode*/ 4 + /*gid*/ 4;

    case MessageType::Rlcreate:
        // size[4] Rlcreate tag[2] qid[13] iounit[4]
        return HeaderSize + /*qid*/ 13 + /*iounit*/ 4;

    case MessageType::Tsymlink:
        // size[4] Tsymlink tag[2] fid[4] name[s] symtgt[s] gid[4]
        // Excludes: name string data, symtgt string data
        return HeaderSize + /*fid*/ 4 + /*name*/ 2 + /*symtgt*/ 2 + /*gid*/ 4;

    case MessageType::Rsymlink:
        // size[4] Rsymlink tag[2] qid[13]
        return HeaderSize + /*qid*/ 13;

    case MessageType::Tmknod:
        // size[4] Tmknod tag[2] dfid[4] name[s] mode[4] major[4] minor[4] gid[4]
        // Excludes: name string data
        return HeaderSize + /*dfid*/ 4 + /*name*/ 2 + /*mode*/ 4 + /*major*/ 4 + /*minor*/ 4 + /*gid*/ 4;

    case MessageType::Rmknod:
        // size[4] Rmknod tag[2] qid[13]
        return HeaderSize + /*qid*/ 13;

    case MessageType::Trename:
        // size[4] Trename tag[2] fid[4] dfid[4] name[s]
        // Excludes: name string data
        return HeaderSize + /*fid*/ 4 + /*dfid*/ 4 + /*name*/ 2;

    case MessageType::Rrename:
        // size[4] Rrename tag[2]
        return HeaderSize;

    case MessageType::Treadlink:
        // size[4] Treadlink tag[2] fid[4]
        return HeaderSize + /*fid*/ 4;

    case MessageType::Rreadlink:
        // size[4] Rreadlink tag[2] target[s]
        // Excludes: target string data
        return HeaderSize + /*target*/ 2;

    case MessageType::Tgetattr:
        // size[4] Tgetattr tag[2] fid[4] request_mask[8]
        return HeaderSize + /*fid*/ 4 + /*request_mask*/ 8;

    case MessageType::Rgetattr:
        // size[4] Rgetattr tag[2] valid[8] qid[13] mode[4] uid[4] gid[4] nlink[8] rdev[8] size[8] blksize[8] blocks[8] atime_sec[8] atime_nsec[8] mtime_sec[8] mtime_nsec[8] ctime_sec[8] ctime_nsec[8] btime_sec[8] btime_nsec[8] gen[8] data_version[8]
        return HeaderSize + /*valid*/ 8 + /*qid*/ 13 + /*mode*/ 4 + /*uid*/ 4 + /*gid*/ 4 + /*nlink*/ 8 + /*rdev*/ 8 + /*size*/ 8 +
               /*blksize*/ 8 + /*blocks*/ 8 + /*atime_sec*/ 8 + /*atime_nsec*/ 8 + /*mtime_sec*/ 8 + /*mtime_nsec*/ 8 +
               /*ctime_sec*/ 8 + /*ctime_nsec*/ 8 + /*btime_sec*/ 8 + /*btime_nsec*/ 8 + /*gen*/ 8 + /*data_version*/ 8;

    case MessageType::Tsetattr:
        // size[4] Tsetattr tag[2] fid[4] valid[4] mode[4] uid[4] gid[4] size[8] atime_sec[8] atime_nsec[8] mtime_sec[8] mtime_nsec[8]
        return HeaderSize + /*fid*/ 4 + /*valid*/ 4 + /*mode*/ 4 + /*uid*/ 4 + /*gid*/ 4 + /*size*/ 8 + /*atime_sec*/ 8 +
               /*atime_nsec*/ 8 + /*mtime_sec*/ 8 + /*mtime_nsec*/ 8;

    case MessageType::Rsetattr:
        // size[4] Rsetattr tag[2]
        return HeaderSize;

    case MessageType::Txattrwalk:
        // size[4] Txattrwalk tag[2] fid[4] newfid[4] name[s]
        // Excludes: name string data
        return HeaderSize + /*fid*/ 4 + /*newfid*/ 4 + /*name*/ 2;

    case MessageType::Rxattrwalk:
        // size[4] Rxattrwalk tag[2] size[8]
        return HeaderSize + /*size*/ 8;

    case MessageType::Txattrcreate:
        // size[4] Txattrcreate tag[2] fid[4] name[s] attr_size[8] flags[4]
        // Excludes: name string data
        return HeaderSize + /*fid*/ 4 + /*name*/ 2 + /*attr_size*/ 8 + /*flags*/ 4;

    case MessageType::Rxattrcreate:
        // size[4] Rxattrcreate tag[2]
        return HeaderSize;

    case MessageType::Treaddir:
        // size[4] Treaddir tag[2] fid[4] offset[8] count[4]
        return HeaderSize + /*fid*/ 4 + /*offset*/ 8 + /*count*/ 4;

    case MessageType::Rreaddir:
        // size[4] Rreaddir tag[2] count[4] data[count]
        // Excludes: data
        return HeaderSize + /*count*/ 4;

    case MessageType::Tfsync:
        // size[4] Tfsync tag[2] fid[4]
        return HeaderSize + /*fid*/ 4;

    case MessageType::Rfsync:
        // size[4] Rfsync tag[2]
        return HeaderSize;

    case MessageType::Tlock:
        // size[4] Tlock tag[2] fid[4] type[1] flags[4] start[8] length[8] proc_id[4] client_id[s]
        // Excludes: client_id string data
        return HeaderSize + /*fid*/ 4 + /*type*/ 1 + /*flags*/ 4 + /*start*/ 8 + /*length*/ 8 + /*proc_id*/ 4 + /*client_id*/ 2;

    case MessageType::Rlock:
        // size[4] Rlock tag[2] status[1]
        return HeaderSize + /*status*/ 1;

    case MessageType::Tgetlock:
        // size[4] Tgetlock tag[2] fid[4] type[1] start[8] length[8] proc_id[4] client_id[s]
        // Excludes: client_id string data
        return HeaderSize + /*fid*/ 4 + /*type*/ 1 + /*start*/ 8 + /*length*/ 8 + /*proc_id*/ 4 + /*client_id*/ 2;

    case MessageType::Rgetlock:
        // size[4] Rgetlock tag[2] type[1] start[8] length[8] proc_id[4] client_id[s]
        // Excludes: client_id string data
        return HeaderSize + /*type*/ 1 + /*start*/ 8 + /*length*/ 8 + /*proc_id*/ 4 + /*client_id*/ 2;

    case MessageType::Tlink:
        // size[4] Tlink tag[2] dfid[4] fid[4] name[s]
        // Excludes: name string data
        return HeaderSize + /*dfid*/ 4 + /*fid*/ 4 + /*name*/ 2;

    case MessageType::Rlink:
        // size[4] Rlink tag[2]
        return HeaderSize;

    case MessageType::Tmkdir:
        // size[4] Tmkdir tag[2] dfid[4] name[s] mode[4] gid[4]
        // Excludes: name string data
        return HeaderSize + /*dfid*/ 4 + /*name*/ 2 + /*mode*/ 4 + /*gid*/ 4;

    case MessageType::Rmkdir:
        // size[4] Rmkdir tag[2] qid[13]
        return HeaderSize + /*qid*/ 13;

    case MessageType::Trenameat:
        // size[4] Trenameat tag[2] olddirfid[4] oldname[s] newdirfid[4] newname[s]
        // Excludes: oldname string data, newname string data
        return HeaderSize + /*olddirfid*/ 4 + /*oldname*/ 2 + /*newdirfid*/ 4 + /*newname*/ 2;

    case MessageType::Rrenameat:
        // size[4] Rrenameat tag[2]
        return HeaderSize;

    case MessageType::Tunlinkat:
        // size[4] Tunlinkat tag[2] dirfd[4] name[s] flags[4]
        // Excludes: name string data
        return HeaderSize + /*dirfd*/ 4 + /*name*/ 2 + /*flags*/ 4;

    case MessageType::Runlinkat:
        // size[4] Runlinkat tag[2]
        return HeaderSize;

    case MessageType::Taccess:
        // size[4] Taccess tag[2] fid[4] flags[4]
        return HeaderSize + /*fid*/ 4 + /*flags*/ 4;

    case MessageType::Raccess:
        // size[4] Raccess tag[2]
        return HeaderSize;

    case MessageType::Twreaddir:
        // size[4] Twreaddir tag[2] fid[4] offset[8] count[4]
        return HeaderSize + /*fid*/ 4 + /*offset*/ 8 + /*count*/ 4;

    case MessageType::Rwreaddir:
        // size[4] Rwreaddir tag[2] count[4] data[count]
        // Excludes: data
        return HeaderSize + /*count*/ 4;

    case MessageType::Twopen:
        // size[4] Twopen tag[2] fid[4] newfid[4] flags[4] wflags[4] mode[4] gid[4] attr_mask[8] nwname[2] nwname*(wname[s])
        // Excludes: repeated elements
        return HeaderSize + /*fid*/ 4 + /*newfid*/ 4 + /*flags*/ 4 + /*wflags*/ 4 + /*mode*/ 4 + /*gid*/ 4 + /*attr_mask*/ 8 + /*nwname*/ 2;

    case MessageType::Rwopen:
        // size[4] Rwopen tag[2] status[1] walked[2] qid[13] symlink_target[s] iounit[4] mode[4] uid[4] gid[4] nlink[8] rdev[8]
        // size[8] blksize[8] blocks[8] atime_sec[8] atime_nsec[8] mtime_sec[8] mtime_nsec[8] ctime_sec[8] ctime_nsec[8]
        // btime_sec[8] btime_nsec[8] gen[8] data_version[8] Excludes: symlink_target string data
        return HeaderSize + /*status*/ 1 + /*walked*/ 2 + /*qid*/ 13 + /*symlink_target*/ 2 + /*iounit*/ 4 + /*mode*/ 4 +
               /*uid*/ 4 + /*gid*/ 4 + /*nlink*/ 8 + /*rdev*/ 8 + /*size*/ 8 + /*blksize*/ 8 + /*blocks*/ 8 + /*atime_sec*/ 8 +
               /*atime_nsec*/ 8 + /*mtime_sec*/ 8 + /*mtime_nsec*/ 8 + /*ctime_sec*/ 8 + /*ctime_nsec*/ 8 + /*btime_sec*/ 8 +
               /*btime_nsec*/ 8 + /*gen*/ 8 + /*data_version*/ 8;

    default:
        return 0;
    }
}

} // namespace p9fs

/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    p9log.h

Abstract:

    This file contains the plan9 logging implementation.

--*/

#pragma once

namespace p9fs {

// Outputs a log message to stdout with the contents of the specified message.
inline void TraceLogMessage([[maybe_unused]] gsl::span<const gsl::byte> message)
{

    if (!Plan9TraceLoggingProvider::IsEnabled(TRACE_LEVEL_VERBOSE))
    {
        return;
    }

    SpanReader reader{message};
    reader.U32(); // size (unused)
    auto messageType = static_cast<MessageType>(reader.U8());
    auto tag = reader.U16();
    LogMessageBuilder text;

    switch (messageType)
    {
    case MessageType::Tversion:
    {
        // size[4] Tversion tag[2] msize[4] version[s]
        text.AddName(">>Tversion");
        text.AddField("tag", tag);
        auto msize = reader.U32();
        text.AddField("msize", msize);
        auto version = reader.String();
        text.AddField("version", version);
        break;
    }

    case MessageType::Rversion:
    {
        // size[4] Rversion tag[2] msize[4] version[s]
        text.AddName("<<Rversion");
        text.AddField("tag", tag);
        auto msize = reader.U32();
        text.AddField("msize", msize);
        auto version = reader.String();
        text.AddField("version", version);
        break;
    }

    case MessageType::Tflush:
    {
        // size[4] Tflush tag[2] oldtag[2]
        text.AddName(">>Tflush");
        text.AddField("tag", tag);
        auto oldtag = reader.U16();
        text.AddField("oldtag", oldtag);
        break;
    }

    case MessageType::Rflush:
    {
        // size[4] Rflush tag[2]
        text.AddName("<<Rflush");
        text.AddField("tag", tag);
        break;
    }

    case MessageType::Twalk:
    {
        // size[4] Twalk tag[2] fid[4] newfid[4] nwname[2] nwname*(wname[s])
        text.AddName(">>Twalk");
        text.AddField("tag", tag);
        auto fid = reader.U32();
        text.AddField("fid", fid);
        auto newfid = reader.U32();
        text.AddField("newfid", newfid);
        auto nwname = reader.U16();
        text.AddField("nwname", nwname);
        for (UINT32 i = 0; i < nwname; ++i)
        {
            auto wname = reader.String();
            text.AddValue(wname);
        }
        break;
    }

    case MessageType::Rwalk:
    {
        // size[4] Rwalk tag[2] nwqid[2] nwqid*(wqid[13])
        text.AddName("<<Rwalk");
        text.AddField("tag", tag);
        auto nwqid = reader.U16();
        text.AddField("nwqid", nwqid);
        for (UINT32 i = 0; i < nwqid; ++i)
        {
            auto wqid = reader.Qid();
            text.AddValue(wqid);
        }
        break;
    }

    case MessageType::Tread:
    {
        // size[4] Tread tag[2] fid[4] offset[8] count[4]
        text.AddName(">>Tread");
        text.AddField("tag", tag);
        auto fid = reader.U32();
        text.AddField("fid", fid);
        auto offset = reader.U64();
        text.AddField("offset", offset);
        auto count = reader.U32();
        text.AddField("count", count);
        break;
    }

    case MessageType::Rread:
    {
        // size[4] Rread tag[2] count[4] data[count]
        text.AddName("<<Rread");
        text.AddField("tag", tag);
        auto count = reader.U32();
        text.AddField("count", count);
        break;
    }

    case MessageType::Twrite:
    {
        // size[4] Twrite tag[2] fid[4] offset[8] count[4] data[count]
        text.AddName(">>Twrite");
        text.AddField("tag", tag);
        auto fid = reader.U32();
        text.AddField("fid", fid);
        auto offset = reader.U64();
        text.AddField("offset", offset);
        auto count = reader.U32();
        text.AddField("count", count);
        break;
    }

    case MessageType::Rwrite:
    {
        // size[4] Rwrite tag[2] count[4]
        text.AddName("<<Rwrite");
        text.AddField("tag", tag);
        auto count = reader.U32();
        text.AddField("count", count);
        break;
    }

    case MessageType::Tclunk:
    {
        // size[4] Tclunk tag[2] fid[4]
        text.AddName(">>Tclunk");
        text.AddField("tag", tag);
        auto fid = reader.U32();
        text.AddField("fid", fid);
        break;
    }

    case MessageType::Rclunk:
    {
        // size[4] Rclunk tag[2]
        text.AddName("<<Rclunk");
        text.AddField("tag", tag);
        break;
    }

    case MessageType::Tremove:
    {
        // size[4] Tremove tag[2] fid[4]
        text.AddName(">>Tremove");
        text.AddField("tag", tag);
        auto fid = reader.U32();
        text.AddField("fid", fid);
        break;
    }

    case MessageType::Rremove:
    {
        // size[4] Rremove tag[2]
        text.AddName("<<Rremove");
        text.AddField("tag", tag);
        break;
    }

    case MessageType::Tauth:
    {
        // size[4] Tauth tag[2] afid[4] uname[s] aname[s] n_uname[4]
        text.AddName(">>Tauth");
        text.AddField("tag", tag);
        auto afid = reader.U32();
        text.AddField("afid", afid);
        auto uname = reader.String();
        text.AddField("uname", uname);
        auto aname = reader.String();
        text.AddField("aname", aname);
        auto n_uname = reader.U32();
        text.AddField("n_uname", n_uname);
        break;
    }

    case MessageType::Rauth:
    {
        // size[4] Rauth tag[2] aqid[13]
        text.AddName("<<Rauth");
        text.AddField("tag", tag);
        auto aqid = reader.Qid();
        text.AddField("aqid", aqid);
        break;
    }

    case MessageType::Tattach:
    {
        // size[4] Tattach tag[2] fid[4] afid[4] uname[s] aname[s] n_uname[4]
        text.AddName(">>Tattach");
        text.AddField("tag", tag);
        auto fid = reader.U32();
        text.AddField("fid", fid);
        auto afid = reader.U32();
        text.AddField("afid", afid);
        auto uname = reader.String();
        text.AddField("uname", uname);
        auto aname = reader.String();
        text.AddField("aname", aname);
        auto n_uname = reader.U32();
        text.AddField("n_uname", n_uname);
        break;
    }

    case MessageType::Rattach:
    {
        // size[4] Rattach tag[2] qid[13]
        text.AddName("<<Rattach");
        text.AddField("tag", tag);
        auto qid = reader.Qid();
        text.AddField("qid", qid);
        break;
    }

    case MessageType::Rlerror:
    {
        // size[4] Rlerror tag[2] ecode[4]
        text.AddName("<<Rlerror");
        text.AddField("tag", tag);
        auto ecode = reader.U32();
        text.AddField("ecode", ecode);
        break;
    }

    case MessageType::Tstatfs:
    {
        // size[4] Tstatfs tag[2] fid[4]
        text.AddName(">>Tstatfs");
        text.AddField("tag", tag);
        auto fid = reader.U32();
        text.AddField("fid", fid);
        break;
    }

    case MessageType::Rstatfs:
    {
        // size[4] Rstatfs tag[2] type[4] bsize[4] blocks[8] bfree[8] bavail[8] files[8] ffree[8] fsid[8] namelen[4]
        text.AddName("<<Rstatfs");
        text.AddField("tag", tag);
        auto type = reader.U32();
        text.AddField("type", type);
        auto bsize = reader.U32();
        text.AddField("bsize", bsize);
        auto blocks = reader.U64();
        text.AddField("blocks", blocks);
        auto bfree = reader.U64();
        text.AddField("bfree", bfree);
        auto bavail = reader.U64();
        text.AddField("bavail", bavail);
        auto files = reader.U64();
        text.AddField("files", files);
        auto ffree = reader.U64();
        text.AddField("ffree", ffree);
        auto fsid = reader.U64();
        text.AddField("fsid", fsid);
        auto namelen = reader.U32();
        text.AddField("namelen", namelen);
        break;
    }

    case MessageType::Tlopen:
    {
        // size[4] Tlopen tag[2] fid[4] flags[4]
        text.AddName(">>Tlopen");
        text.AddField("tag", tag);
        auto fid = reader.U32();
        text.AddField("fid", fid);
        auto flags = reader.U32();
        text.AddField("flags", flags);
        break;
    }

    case MessageType::Rlopen:
    {
        // size[4] Rlopen tag[2] qid[13] iounit[4]
        text.AddName("<<Rlopen");
        text.AddField("tag", tag);
        auto qid = reader.Qid();
        text.AddField("qid", qid);
        auto iounit = reader.U32();
        text.AddField("iounit", iounit);
        break;
    }

    case MessageType::Tlcreate:
    {
        // size[4] Tlcreate tag[2] fid[4] name[s] flags[4] mode[4] gid[4]
        text.AddName(">>Tlcreate");
        text.AddField("tag", tag);
        auto fid = reader.U32();
        text.AddField("fid", fid);
        auto name = reader.String();
        text.AddField("name", name);
        auto flags = reader.U32();
        text.AddField("flags", flags);
        auto mode = reader.U32();
        text.AddField("mode", mode);
        auto gid = reader.U32();
        text.AddField("gid", gid);
        break;
    }

    case MessageType::Rlcreate:
    {
        // size[4] Rlcreate tag[2] qid[13] iounit[4]
        text.AddName("<<Rlcreate");
        text.AddField("tag", tag);
        auto qid = reader.Qid();
        text.AddField("qid", qid);
        auto iounit = reader.U32();
        text.AddField("iounit", iounit);
        break;
    }

    case MessageType::Tsymlink:
    {
        // size[4] Tsymlink tag[2] fid[4] name[s] symtgt[s] gid[4]
        text.AddName(">>Tsymlink");
        text.AddField("tag", tag);
        auto fid = reader.U32();
        text.AddField("fid", fid);
        auto name = reader.String();
        text.AddField("name", name);
        auto symtgt = reader.String();
        text.AddField("symtgt", symtgt);
        auto gid = reader.U32();
        text.AddField("gid", gid);
        break;
    }

    case MessageType::Rsymlink:
    {
        // size[4] Rsymlink tag[2] qid[13]
        text.AddName("<<Rsymlink");
        text.AddField("tag", tag);
        auto qid = reader.Qid();
        text.AddField("qid", qid);
        break;
    }

    case MessageType::Tmknod:
    {
        // size[4] Tmknod tag[2] dfid[4] name[s] mode[4] major[4] minor[4] gid[4]
        text.AddName(">>Tmknod");
        text.AddField("tag", tag);
        auto dfid = reader.U32();
        text.AddField("dfid", dfid);
        auto name = reader.String();
        text.AddField("name", name);
        auto mode = reader.U32();
        text.AddField("mode", mode);
        auto major = reader.U32();
        text.AddField("major", major);
        auto minor = reader.U32();
        text.AddField("minor", minor);
        auto gid = reader.U32();
        text.AddField("gid", gid);
        break;
    }

    case MessageType::Rmknod:
    {
        // size[4] Rmknod tag[2] qid[13]
        text.AddName("<<Rmknod");
        text.AddField("tag", tag);
        auto qid = reader.Qid();
        text.AddField("qid", qid);
        break;
    }

    case MessageType::Trename:
    {
        // size[4] Trename tag[2] fid[4] dfid[4] name[s]
        text.AddName(">>Trename");
        text.AddField("tag", tag);
        auto fid = reader.U32();
        text.AddField("fid", fid);
        auto dfid = reader.U32();
        text.AddField("dfid", dfid);
        auto name = reader.String();
        text.AddField("name", name);
        break;
    }

    case MessageType::Rrename:
    {
        // size[4] Rrename tag[2]
        text.AddName("<<Rrename");
        text.AddField("tag", tag);
        break;
    }

    case MessageType::Treadlink:
    {
        // size[4] Treadlink tag[2] fid[4]
        text.AddName(">>Treadlink");
        text.AddField("tag", tag);
        auto fid = reader.U32();
        text.AddField("fid", fid);
        break;
    }

    case MessageType::Rreadlink:
    {
        // size[4] Rreadlink tag[2] target[s]
        text.AddName("<<Rreadlink");
        text.AddField("tag", tag);
        auto target = reader.String();
        text.AddField("target", target);
        break;
    }

    case MessageType::Tgetattr:
    {
        // size[4] Tgetattr tag[2] fid[4] request_mask[8]
        text.AddName(">>Tgetattr");
        text.AddField("tag", tag);
        auto fid = reader.U32();
        text.AddField("fid", fid);
        auto request_mask = reader.U64();
        text.AddField("request_mask", request_mask);
        break;
    }

    case MessageType::Rgetattr:
    {
        // size[4] Rgetattr tag[2] valid[8] qid[13] mode[4] uid[4] gid[4] nlink[8] rdev[8] size[8] blksize[8] blocks[8] atime_sec[8] atime_nsec[8] mtime_sec[8] mtime_nsec[8] ctime_sec[8] ctime_nsec[8] btime_sec[8] btime_nsec[8] gen[8] data_version[8]
        text.AddName("<<Rgetattr");
        text.AddField("tag", tag);
        auto valid = reader.U64();
        text.AddField("valid", valid);
        auto qid = reader.Qid();
        text.AddField("qid", qid);
        auto mode = reader.U32();
        text.AddField("mode", mode);
        auto uid = reader.U32();
        text.AddField("uid", uid);
        auto gid = reader.U32();
        text.AddField("gid", gid);
        auto nlink = reader.U64();
        text.AddField("nlink", nlink);
        auto rdev = reader.U64();
        text.AddField("rdev", rdev);
        auto size = reader.U64();
        text.AddField("size", size);
        auto blksize = reader.U64();
        text.AddField("blksize", blksize);
        auto blocks = reader.U64();
        text.AddField("blocks", blocks);
        auto atime_sec = reader.U64();
        text.AddField("atime_sec", atime_sec);
        auto atime_nsec = reader.U64();
        text.AddField("atime_nsec", atime_nsec);
        auto mtime_sec = reader.U64();
        text.AddField("mtime_sec", mtime_sec);
        auto mtime_nsec = reader.U64();
        text.AddField("mtime_nsec", mtime_nsec);
        auto ctime_sec = reader.U64();
        text.AddField("ctime_sec", ctime_sec);
        auto ctime_nsec = reader.U64();
        text.AddField("ctime_nsec", ctime_nsec);
        auto btime_sec = reader.U64();
        text.AddField("btime_sec", btime_sec);
        auto btime_nsec = reader.U64();
        text.AddField("btime_nsec", btime_nsec);
        auto gen = reader.U64();
        text.AddField("gen", gen);
        auto data_version = reader.U64();
        text.AddField("data_version", data_version);
        break;
    }

    case MessageType::Tsetattr:
    {
        // size[4] Tsetattr tag[2] fid[4] valid[4] mode[4] uid[4] gid[4] size[8] atime_sec[8] atime_nsec[8] mtime_sec[8] mtime_nsec[8]
        text.AddName(">>Tsetattr");
        text.AddField("tag", tag);
        auto fid = reader.U32();
        text.AddField("fid", fid);
        auto valid = reader.U32();
        text.AddField("valid", valid);
        auto mode = reader.U32();
        text.AddField("mode", mode);
        auto uid = reader.U32();
        text.AddField("uid", uid);
        auto gid = reader.U32();
        text.AddField("gid", gid);
        auto size = reader.U64();
        text.AddField("size", size);
        auto atime_sec = reader.U64();
        text.AddField("atime_sec", atime_sec);
        auto atime_nsec = reader.U64();
        text.AddField("atime_nsec", atime_nsec);
        auto mtime_sec = reader.U64();
        text.AddField("mtime_sec", mtime_sec);
        auto mtime_nsec = reader.U64();
        text.AddField("mtime_nsec", mtime_nsec);
        break;
    }

    case MessageType::Rsetattr:
    {
        // size[4] Rsetattr tag[2]
        text.AddName("<<Rsetattr");
        text.AddField("tag", tag);
        break;
    }

    case MessageType::Txattrwalk:
    {
        // size[4] Txattrwalk tag[2] fid[4] newfid[4] name[s]
        text.AddName(">>Txattrwalk");
        text.AddField("tag", tag);
        auto fid = reader.U32();
        text.AddField("fid", fid);
        auto newfid = reader.U32();
        text.AddField("newfid", newfid);
        auto name = reader.String();
        text.AddField("name", name);
        break;
    }

    case MessageType::Rxattrwalk:
    {
        // size[4] Rxattrwalk tag[2] size[8]
        text.AddName("<<Rxattrwalk");
        text.AddField("tag", tag);
        auto size = reader.U64();
        text.AddField("size", size);
        break;
    }

    case MessageType::Txattrcreate:
    {
        // size[4] Txattrcreate tag[2] fid[4] name[s] attr_size[8] flags[4]
        text.AddName(">>Txattrcreate");
        text.AddField("tag", tag);
        auto fid = reader.U32();
        text.AddField("fid", fid);
        auto name = reader.String();
        text.AddField("name", name);
        auto attr_size = reader.U64();
        text.AddField("attr_size", attr_size);
        auto flags = reader.U32();
        text.AddField("flags", flags);
        break;
    }

    case MessageType::Rxattrcreate:
    {
        // size[4] Rxattrcreate tag[2]
        text.AddName("<<Rxattrcreate");
        text.AddField("tag", tag);
        break;
    }

    case MessageType::Treaddir:
    {
        // size[4] Treaddir tag[2] fid[4] offset[8] count[4]
        text.AddName(">>Treaddir");
        text.AddField("tag", tag);
        auto fid = reader.U32();
        text.AddField("fid", fid);
        auto offset = reader.U64();
        text.AddField("offset", offset);
        auto count = reader.U32();
        text.AddField("count", count);
        break;
    }

    case MessageType::Rreaddir:
    {
        // size[4] Rreaddir tag[2] count[4] data[count]
        text.AddName("<<Rreaddir");
        text.AddField("tag", tag);
        auto count = reader.U32();
        text.AddField("count", count);
        break;
    }

    case MessageType::Tfsync:
    {
        // size[4] Tfsync tag[2] fid[4]
        text.AddName(">>Tfsync");
        text.AddField("tag", tag);
        auto fid = reader.U32();
        text.AddField("fid", fid);
        break;
    }

    case MessageType::Rfsync:
    {
        // size[4] Rfsync tag[2]
        text.AddName("<<Rfsync");
        text.AddField("tag", tag);
        break;
    }

    case MessageType::Tlock:
    {
        // size[4] Tlock tag[2] fid[4] type[1] flags[4] start[8] length[8] proc_id[4] client_id[s]
        text.AddName(">>Tlock");
        text.AddField("tag", tag);
        auto fid = reader.U32();
        text.AddField("fid", fid);
        auto type = reader.U8();
        text.AddField("type", type);
        auto flags = reader.U32();
        text.AddField("flags", flags);
        auto start = reader.U64();
        text.AddField("start", start);
        auto length = reader.U64();
        text.AddField("length", length);
        auto proc_id = reader.U32();
        text.AddField("proc_id", proc_id);
        auto client_id = reader.String();
        text.AddField("client_id", client_id);
        break;
    }

    case MessageType::Rlock:
    {
        // size[4] Rlock tag[2] status[1]
        text.AddName("<<Rlock");
        text.AddField("tag", tag);
        auto status = reader.U8();
        text.AddField("status", status);
        break;
    }

    case MessageType::Tgetlock:
    {
        // size[4] Tgetlock tag[2] fid[4] type[1] start[8] length[8] proc_id[4] client_id[s]
        text.AddName(">>Tgetlock");
        text.AddField("tag", tag);
        auto fid = reader.U32();
        text.AddField("fid", fid);
        auto type = reader.U8();
        text.AddField("type", type);
        auto start = reader.U64();
        text.AddField("start", start);
        auto length = reader.U64();
        text.AddField("length", length);
        auto proc_id = reader.U32();
        text.AddField("proc_id", proc_id);
        auto client_id = reader.String();
        text.AddField("client_id", client_id);
        break;
    }

    case MessageType::Rgetlock:
    {
        // size[4] Rgetlock tag[2] type[1] start[8] length[8] proc_id[4] client_id[s]
        text.AddName("<<Rgetlock");
        text.AddField("tag", tag);
        auto type = reader.U8();
        text.AddField("type", type);
        auto start = reader.U64();
        text.AddField("start", start);
        auto length = reader.U64();
        text.AddField("length", length);
        auto proc_id = reader.U32();
        text.AddField("proc_id", proc_id);
        auto client_id = reader.String();
        text.AddField("client_id", client_id);
        break;
    }

    case MessageType::Tlink:
    {
        // size[4] Tlink tag[2] dfid[4] fid[4] name[s]
        text.AddName(">>Tlink");
        text.AddField("tag", tag);
        auto dfid = reader.U32();
        text.AddField("dfid", dfid);
        auto fid = reader.U32();
        text.AddField("fid", fid);
        auto name = reader.String();
        text.AddField("name", name);
        break;
    }

    case MessageType::Rlink:
    {
        // size[4] Rlink tag[2]
        text.AddName("<<Rlink");
        text.AddField("tag", tag);
        break;
    }

    case MessageType::Tmkdir:
    {
        // size[4] Tmkdir tag[2] dfid[4] name[s] mode[4] gid[4]
        text.AddName(">>Tmkdir");
        text.AddField("tag", tag);
        auto dfid = reader.U32();
        text.AddField("dfid", dfid);
        auto name = reader.String();
        text.AddField("name", name);
        auto mode = reader.U32();
        text.AddField("mode", mode);
        auto gid = reader.U32();
        text.AddField("gid", gid);
        break;
    }

    case MessageType::Rmkdir:
    {
        // size[4] Rmkdir tag[2] qid[13]
        text.AddName("<<Rmkdir");
        text.AddField("tag", tag);
        auto qid = reader.Qid();
        text.AddField("qid", qid);
        break;
    }

    case MessageType::Trenameat:
    {
        // size[4] Trenameat tag[2] olddirfid[4] oldname[s] newdirfid[4] newname[s]
        text.AddName(">>Trenameat");
        text.AddField("tag", tag);
        auto olddirfid = reader.U32();
        text.AddField("olddirfid", olddirfid);
        auto oldname = reader.String();
        text.AddField("oldname", oldname);
        auto newdirfid = reader.U32();
        text.AddField("newdirfid", newdirfid);
        auto newname = reader.String();
        text.AddField("newname", newname);
        break;
    }

    case MessageType::Rrenameat:
    {
        // size[4] Rrenameat tag[2]
        text.AddName("<<Rrenameat");
        text.AddField("tag", tag);
        break;
    }

    case MessageType::Tunlinkat:
    {
        // size[4] Tunlinkat tag[2] dirfd[4] name[s] flags[4]
        text.AddName(">>Tunlinkat");
        text.AddField("tag", tag);
        auto dirfd = reader.U32();
        text.AddField("dirfd", dirfd);
        auto name = reader.String();
        text.AddField("name", name);
        auto flags = reader.U32();
        text.AddField("flags", flags);
        break;
    }

    case MessageType::Runlinkat:
    {
        // size[4] Runlinkat tag[2]
        text.AddName("<<Runlinkat");
        text.AddField("tag", tag);
        break;
    }

    case MessageType::Taccess:
    {
        // size[4] Taccess tag[2] fid[4] flags[4]
        text.AddName(">>Taccess");
        text.AddField("tag", tag);
        auto fid = reader.U32();
        text.AddField("fid", fid);
        auto flags = reader.U32();
        text.AddField("flags", flags);
        break;
    }

    case MessageType::Raccess:
    {
        // size[4] Raccess tag[2]
        text.AddName("<<Raccess");
        text.AddField("tag", tag);
        break;
    }

    case MessageType::Twreaddir:
    {
        // size[4] Twreaddir tag[2] fid[4] offset[8] count[4]
        text.AddName(">>Twreaddir");
        text.AddField("tag", tag);
        auto fid = reader.U32();
        text.AddField("fid", fid);
        auto offset = reader.U64();
        text.AddField("offset", offset);
        auto count = reader.U32();
        text.AddField("count", count);
        break;
    }

    case MessageType::Rwreaddir:
    {
        // size[4] Rwreaddir tag[2] count[4] data[count]
        text.AddName("<<Rwreaddir");
        text.AddField("tag", tag);
        auto count = reader.U32();
        text.AddField("count", count);
        break;
    }

    case MessageType::Twopen:
    {
        // size[4] Twopen tag[2] fid[4] newfid[4] flags[4] wflags[4] mode[4] gid[4] attr_mask[8] nwname[2] nwname*(wname[s])
        text.AddName(">>Twopen");
        text.AddField("tag", tag);
        auto fid = reader.U32();
        text.AddField("fid", fid);
        auto newfid = reader.U32();
        text.AddField("newfid", newfid);
        auto flags = reader.U32();
        text.AddField("flags", flags);
        auto wflags = reader.U32();
        text.AddField("wflags", wflags);
        auto mode = reader.U32();
        text.AddField("mode", mode);
        auto gid = reader.U32();
        text.AddField("gid", gid);
        auto attr_mask = reader.U64();
        text.AddField("attr_mask", attr_mask);
        auto nwname = reader.U16();
        text.AddField("nwname", nwname);
        for (UINT32 i = 0; i < nwname; ++i)
        {
            auto wname = reader.String();
            text.AddValue(wname);
        }
        break;
    }

    case MessageType::Rwopen:
    {
        // size[4] Rwopen tag[2] status[1] walked[2] qid[13] symlink_target[s] iounit[4] mode[4] uid[4] gid[4] nlink[8] rdev[8] size[8] blksize[8] blocks[8] atime_sec[8] atime_nsec[8] mtime_sec[8] mtime_nsec[8] ctime_sec[8] ctime_nsec[8] btime_sec[8] btime_nsec[8] gen[8] data_version[8]
        text.AddName("<<Rwopen");
        text.AddField("tag", tag);
        auto status = reader.U8();
        text.AddField("status", status);
        auto walked = reader.U16();
        text.AddField("walked", walked);
        auto qid = reader.Qid();
        text.AddField("qid", qid);
        auto symlink_target = reader.String();
        text.AddField("symlink_target", symlink_target);
        auto iounit = reader.U32();
        text.AddField("iounit", iounit);
        auto mode = reader.U32();
        text.AddField("mode", mode);
        auto uid = reader.U32();
        text.AddField("uid", uid);
        auto gid = reader.U32();
        text.AddField("gid", gid);
        auto nlink = reader.U64();
        text.AddField("nlink", nlink);
        auto rdev = reader.U64();
        text.AddField("rdev", rdev);
        auto size = reader.U64();
        text.AddField("size", size);
        auto blksize = reader.U64();
        text.AddField("blksize", blksize);
        auto blocks = reader.U64();
        text.AddField("blocks", blocks);
        auto atime_sec = reader.U64();
        text.AddField("atime_sec", atime_sec);
        auto atime_nsec = reader.U64();
        text.AddField("atime_nsec", atime_nsec);
        auto mtime_sec = reader.U64();
        text.AddField("mtime_sec", mtime_sec);
        auto mtime_nsec = reader.U64();
        text.AddField("mtime_nsec", mtime_nsec);
        auto ctime_sec = reader.U64();
        text.AddField("ctime_sec", ctime_sec);
        auto ctime_nsec = reader.U64();
        text.AddField("ctime_nsec", ctime_nsec);
        auto btime_sec = reader.U64();
        text.AddField("btime_sec", btime_sec);
        auto btime_nsec = reader.U64();
        text.AddField("btime_nsec", btime_nsec);
        auto gen = reader.U64();
        text.AddField("gen", gen);
        auto data_version = reader.U64();
        text.AddField("data_version", data_version);
        break;
    }

    default:
    {
        text.AddName("Unknown");
        text.AddField("tag", tag);
        text.AddField("type", static_cast<UINT32>(messageType));
        break;
    }
    }

    Plan9TraceLoggingProvider::LogMessage(text.String());
}
} // namespace p9fs

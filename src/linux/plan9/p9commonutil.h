// Copyright (C) Microsoft Corporation. All rights reserved.
#pragma once

namespace p9fs::util {

// Writes a StatResult to a SpanWriter using the format used by Rgetattr and Rwreaddir.
inline void SpanWriteStatResult(SpanWriter& writer, const StatResult& stat)
{
    writer.U32(stat.Mode);
    writer.U32(stat.Uid);
    writer.U32(stat.Gid);
    writer.U64(stat.NLink);
    writer.U64(stat.RDev);
    writer.U64(stat.Size);
    writer.U64(stat.BlockSize);
    writer.U64(stat.Blocks);
    writer.U64(stat.AtimeSec);
    writer.U64(stat.AtimeNsec);
    writer.U64(stat.MtimeSec);
    writer.U64(stat.MtimeNsec);
    writer.U64(stat.CtimeSec);
    writer.U64(stat.CtimeNsec);
}

// Writes a directory entry to a span writer, returning whether the entry fit.
inline bool SpanWriteDirectoryEntry(SpanWriter& writer, std::string_view name, const Qid& qid, UINT64 nextOffset, UCHAR type, const StatResult* stat = nullptr)
{
    size_t dirEntrySize = QidSize + sizeof(UINT64) + sizeof(UCHAR) + sizeof(UINT16) + name.size();
    if (stat != nullptr)
    {
        dirEntrySize += StatResultSize;
    }

    if (static_cast<size_t>(writer.Peek().size()) < dirEntrySize)
    {
        return false;
    }

    writer.Qid(qid);
    writer.U64(nextOffset);
    writer.U8(type); // type is bits 12-15 of the file mode
    writer.String(name);
    if (stat != nullptr)
    {
        SpanWriteStatResult(writer, *stat);
    }

    return true;
}

// Determines the QidType to use for a DT_* enumeration value.
inline QidType DirEntryTypeToQidType(int type)
{
    switch (type)
    {
    case LX_DT_DIR:
        return QidType::Directory;

    case LX_DT_LNK:
        return QidType::Symlink;

    default:
        return QidType::File;
    }
}

// Converts a DT_* value to a S_IF* value.
// N.B. These constants uses the same values for the same file types, just shifted by 12 bits to
//      make space for the permission bits.
inline LX_MODE_T DirEntryTypeToMode(int type)
{
    return (type << 12);
}

// Container-like wrapper around LIST_ENTRY based linked lists.
// N.B. It's assumed the value type has an entry named Link of type LIST_ENTRY.
// N.B. This is by no means intended to meet the requirements of a true STL container, but provides
//      enough functionality to at least use a for-each loop.
template <typename T>
class LinkedList
{
public:
    using value_type = T;
    using reference = T&;
    using const_reference = const T&;
    using difference_type = ptrdiff_t;
    using size_type = size_t;

    // Bidirectional forward iterator for the LinkedList class.
    class iterator
    {
    public:
        using iterator_category = std::bidirectional_iterator_tag;
        using value_type = T;
        using difference_type = ptrdiff_t;
        using pointer = T*;
        using reference = T&;

        // Creates an iterator that refers to the specified list entry.
        explicit iterator(PLIST_ENTRY entry) : m_entry{entry}
        {
        }

        // Moves to the next element in the list.
        iterator& operator++()
        {
            m_entry = m_entry->Flink;
            return *this;
        }

        // Moves to the next element in the list.
        iterator operator++(int)
        {
            auto result = *this;
            m_entry = m_entry->Flink;
            return result;
        }

        // Moves to the previous element in the list.
        iterator& operator--()
        {
            m_entry = m_entry->Blink;
            return *this;
        }

        // Moves to the previous element in the list.
        iterator operator--(int)
        {
            auto result = *this;
            m_entry = m_entry->Blink;
            return result;
        }

        // Checks whether two iterators refer to the same entry.
        bool operator==(const iterator& other) const
        {
            return m_entry == other.m_entry;
        }

        // Checks whether two iterators do not refer to the same entry.
        bool operator!=(const iterator& other) const
        {
            return m_entry != other.m_entry;
        }

        // Returns the value referred to by the iterator.
        reference operator*()
        {
            return *CONTAINING_RECORD(m_entry, T, Link);
        }

    private:
        PLIST_ENTRY m_entry;
    };

    // Initializes a new LinkedList.
    LinkedList()
    {
        InitializeListHead(&m_head);
    }

    // Destroys the LinkedList.
    ~LinkedList()
    {
        // LinkedList doesn't own the items, so it can't clear the list on destruction. Instead,
        // the list should already be cleared.
        WI_ASSERT(IsListEmpty(&m_head));
    }

    // This class is not copyable or moveable.
    // N.B. It could be made moveable, but that would require modifying the list to point to the
    //      new list head, and is not done here.
    LinkedList(const LinkedList&) = delete;
    LinkedList& operator=(const LinkedList&) = delete;
    LinkedList(LinkedList&&) = delete;
    LinkedList& operator=(LinkedList&&) = delete;

    // Inserts a new item into the list.
    void Insert(T& value)
    {
        InsertTailList(&m_head, std::addressof(value.Link));
    }

    // Removes an item from the list.
    // N.B. This could be static, but isn't for ease of invocation and so debug builds can assert
    //      the entry belongs to this list.
    void Remove(T& value)
    {
        WI_ASSERT(Contains(value));

        RemoveEntryList(std::addressof(value.Link));
    }

    // Checks whether the list contains a specific entry.
    bool Contains(const T& value)
    {
        for (auto entry = m_head.Flink; entry != &m_head; entry = entry->Flink)
        {
            if (entry == std::addressof(value.Link))
            {
                return true;
            }
        }

        return false;
    }

    // Returns an iterator to the first element.
    iterator begin()
    {
        return iterator{m_head.Flink};
    }

    // Returns an iterator beyond the last element.
    iterator end()
    {
        return iterator{&m_head};
    }

private:
    LIST_ENTRY m_head;
};

} // namespace p9fs::util

#pragma once

#include <Monarc/Core/Assert.h>
#include <Monarc/Core/Hash.h>
#include <Monarc/Core/Memory/Allocator.h>

#include <cstdlib>
#include <memory>
#include <utility>

namespace Monarc {

/// Open-addressed hash map over an explicit allocator.
///
/// Linear probing with backward-shift deletion, so no tombstones accumulate. Capacity is
/// always a power of two, which makes the bucket index a mask rather than a modulo.
///
/// Copying is deleted, as with Array<T> and String. Iteration order is unspecified and
/// must not be relied on.
template <typename Key, typename Value>
class HashMap {
public:
    struct Entry {
        Key   key;
        Value value;
    };

    explicit HashMap(IAllocator& allocator) : m_allocator(&allocator) {}

    ~HashMap() {
        Clear();
        ReleaseBuffers();
    }

    HashMap(const HashMap&)            = delete;
    HashMap& operator=(const HashMap&) = delete;

    HashMap(HashMap&& other) noexcept
        : m_allocator(other.m_allocator),
          m_entries(other.m_entries),
          m_occupied(other.m_occupied),
          m_size(other.m_size),
          m_capacity(other.m_capacity) {
        other.m_entries  = nullptr;
        other.m_occupied = nullptr;
        other.m_size     = 0;
        other.m_capacity = 0;
    }

    HashMap& operator=(HashMap&& other) noexcept {
        if (this != &other) {
            Clear();
            ReleaseBuffers();
            m_allocator      = other.m_allocator;
            m_entries        = other.m_entries;
            m_occupied       = other.m_occupied;
            m_size           = other.m_size;
            m_capacity       = other.m_capacity;
            other.m_entries  = nullptr;
            other.m_occupied = nullptr;
            other.m_size     = 0;
            other.m_capacity = 0;
        }
        return *this;
    }

    /// Inserts, or replaces the value if the key is already present.
    /// Returns a reference to the stored value.
    Value& Insert(const Key& key, Value value) {
        const usize existing = FindIndex(key);
        if (existing != kInvalidIndex) {
            std::destroy_at(&m_entries[existing].value);
            std::construct_at(&m_entries[existing].value, std::move(value));
            return m_entries[existing].value;
        }

        if (NeedsGrowth()) {
            // `key` may alias storage inside the buffer Grow() is about to move out of and
            // release -- Entry::key is visible through iteration, so a caller can legally
            // pass a reference to an entry the map itself owns (`map.Insert(someEntry.key,
            // v)`). Copying it onto the stack before growing, and using the copy afterward,
            // is the same defence Array<T>::Emplace and String::Append apply to their own
            // by-reference arguments that may alias a buffer about to be freed.
            const Key keyCopy = key;
            Grow();
            return InsertNew(keyCopy, std::move(value));
        }
        return InsertNew(key, std::move(value));
    }

    /// Returns nullptr when the key is absent.
    [[nodiscard]] Value* Find(const Key& key) {
        const usize index = FindIndex(key);
        return index == kInvalidIndex ? nullptr : &m_entries[index].value;
    }

    [[nodiscard]] const Value* Find(const Key& key) const {
        const usize index = FindIndex(key);
        return index == kInvalidIndex ? nullptr : &m_entries[index].value;
    }

    [[nodiscard]] bool Contains(const Key& key) const { return FindIndex(key) != kInvalidIndex; }

    /// Returns true if a key was removed.
    bool Remove(const Key& key) {
        const usize index = FindIndex(key);
        if (index == kInvalidIndex) {
            return false;
        }
        RemoveAt(index);
        return true;
    }

    /// Destroys every entry. Capacity is retained.
    void Clear() {
        for (usize i = 0; i < m_capacity; ++i) {
            if (m_occupied[i]) {
                std::destroy_at(&m_entries[i]);
                m_occupied[i] = false;
            }
        }
        m_size = 0;
    }

    [[nodiscard]] usize Size() const { return m_size; }
    [[nodiscard]] usize Capacity() const { return m_capacity; }
    [[nodiscard]] bool  IsEmpty() const { return m_size == 0; }

    // Forward iteration over occupied entries, in unspecified order.
    class Iterator;
    class ConstIterator;

    [[nodiscard]] Iterator begin() {
        return Iterator(m_entries, m_occupied, FirstOccupiedFrom(0), m_capacity);
    }

    [[nodiscard]] Iterator end() { return Iterator(m_entries, m_occupied, m_capacity, m_capacity); }

    [[nodiscard]] ConstIterator begin() const {
        return ConstIterator(m_entries, m_occupied, FirstOccupiedFrom(0), m_capacity);
    }

    [[nodiscard]] ConstIterator end() const {
        return ConstIterator(m_entries, m_occupied, m_capacity, m_capacity);
    }

    /// Forward iterator over occupied entries. Skips empty slots; never dereferences one.
    class Iterator {
    public:
        Iterator& operator++() {
            do {
                ++m_index;
            } while (m_index < m_capacity && !m_occupied[m_index]);
            return *this;
        }

        [[nodiscard]] Entry& operator*() const { return m_entries[m_index]; }
        [[nodiscard]] Entry* operator->() const { return &m_entries[m_index]; }

        [[nodiscard]] bool operator==(const Iterator& other) const {
            return m_index == other.m_index;
        }
        [[nodiscard]] bool operator!=(const Iterator& other) const {
            return m_index != other.m_index;
        }

    private:
        friend class HashMap;

        Iterator(Entry* entries, const bool* occupied, usize index, usize capacity)
            : m_entries(entries), m_occupied(occupied), m_index(index), m_capacity(capacity) {}

        Entry*      m_entries;
        const bool* m_occupied;
        usize       m_index;
        usize       m_capacity;
    };

    /// Const counterpart of Iterator. A separate type, not a const-qualified Iterator, so
    /// that begin()/end() on a const HashMap cannot yield a mutable reference to an entry.
    class ConstIterator {
    public:
        ConstIterator& operator++() {
            do {
                ++m_index;
            } while (m_index < m_capacity && !m_occupied[m_index]);
            return *this;
        }

        [[nodiscard]] const Entry& operator*() const { return m_entries[m_index]; }
        [[nodiscard]] const Entry* operator->() const { return &m_entries[m_index]; }

        [[nodiscard]] bool operator==(const ConstIterator& other) const {
            return m_index == other.m_index;
        }
        [[nodiscard]] bool operator!=(const ConstIterator& other) const {
            return m_index != other.m_index;
        }

    private:
        friend class HashMap;

        ConstIterator(const Entry* entries, const bool* occupied, usize index, usize capacity)
            : m_entries(entries), m_occupied(occupied), m_index(index), m_capacity(capacity) {}

        const Entry* m_entries;
        const bool*  m_occupied;
        usize        m_index;
        usize        m_capacity;
    };

private:
    /// Largest element count whose byte size still fits in usize, mirroring
    /// Array<T>::kMaxCapacity. The occupancy array (one bool per slot) never needs a
    /// tighter bound: sizeof(bool) == 1 is never larger than sizeof(Entry).
    static constexpr usize kMaxCapacity = static_cast<usize>(-1) / sizeof(Entry);

    static constexpr usize kInitialCapacity = 8;

    /// Sentinel returned by FindIndex for an absent key. Never a valid index: every real
    /// index is below m_capacity, which is bounded well under this value (see kMaxCapacity).
    static constexpr usize kInvalidIndex = static_cast<usize>(-1);

    /// Allocation failure is fatal here, deliberately -- see Array<T>::OnAllocationFailed.
    /// Monarc has no fallible container API yet, and growing silently short would leave
    /// Insert writing past the occupancy array's bounds.
    [[noreturn]] static void OnAllocationFailed() {
        MONARC_DEBUG_BREAK();
        std::abort();
    }

    [[nodiscard]] usize HomeIndex(const Key& key) const {
        return static_cast<usize>(Hash(key)) & (m_capacity - 1);
    }

    /// Places a key already confirmed absent, into a table already confirmed to have room.
    /// Callers must ensure `key` cannot alias a buffer this call releases -- it releases
    /// none, so any reference into the live table remains valid throughout.
    Value& InsertNew(const Key& key, Value value) {
        const usize mask  = m_capacity - 1;
        usize       index = HomeIndex(key);
        while (m_occupied[index]) {
            index = (index + 1) & mask;
        }
        std::construct_at(m_entries + index, key, std::move(value));
        m_occupied[index] = true;
        ++m_size;
        return m_entries[index].value;
    }

    /// Returns the index holding key, or kInvalidIndex. Terminates because the load factor
    /// (see NeedsGrowth) guarantees at least one empty slot exists whenever m_capacity > 0,
    /// and linear probing visits every slot at most once before repeating.
    [[nodiscard]] usize FindIndex(const Key& key) const {
        if (m_capacity == 0) {
            return kInvalidIndex;
        }
        const usize mask  = m_capacity - 1;
        usize       index = HomeIndex(key);
        for (;;) {
            if (!m_occupied[index]) {
                return kInvalidIndex;
            }
            if (m_entries[index].key == key) {
                return index;
            }
            index = (index + 1) & mask;
        }
    }

    /// Backward-shift deletion. Removing the entry at `index` would otherwise strand any
    /// later entry in the same probe chain whose home lies at or before the new hole: such
    /// an entry is no longer reachable by probing forward from its home, because probing
    /// stops at the first empty slot. So each entry in the chain that would be stranded is
    /// shifted back to close the hole, and the hole moves to where that entry used to be;
    /// an entry whose home lies after the hole is left alone, and scanning continues past
    /// it. The chain ends at the first slot that was already empty, which is where the
    /// final hole is left.
    ///
    /// `home` is computed from the *current* m_capacity's mask, so this is correct across a
    /// wraparound at the end of the bucket array: distances are measured modulo m_capacity
    /// (via `& mask`, valid because capacity is a power of two), not by comparing raw
    /// indices, which is exactly the comparison a wraparound would break.
    void RemoveAt(usize index) {
        const usize mask = m_capacity - 1;
        std::destroy_at(&m_entries[index]);
        m_occupied[index] = false;
        --m_size;

        usize hole  = index;
        usize probe = index;
        for (;;) {
            probe = (probe + 1) & mask;
            if (!m_occupied[probe]) {
                break;
            }
            const usize home = HomeIndex(m_entries[probe].key);
            // Distance travelled from home to each candidate position, both mod m_capacity.
            // The entry at `probe` is stranded by the hole precisely when the hole lies on
            // its probe path -- i.e. is strictly closer to home than `probe` itself is.
            const usize distanceToHole  = (hole - home) & mask;
            const usize distanceToProbe = (probe - home) & mask;
            if (distanceToHole < distanceToProbe) {
                std::construct_at(&m_entries[hole], std::move(m_entries[probe]));
                std::destroy_at(&m_entries[probe]);
                m_occupied[hole]  = true;
                m_occupied[probe] = false;
                hole              = probe;
            }
        }
    }

    /// True when inserting one more entry would push the load factor past 3/4. Checked
    /// before every insertion of a new key, never after, so m_capacity > m_size always
    /// holds once any insertion has happened -- the invariant FindIndex's loop relies on to
    /// terminate, and what keeps the map from ever becoming completely full.
    [[nodiscard]] bool NeedsGrowth() const {
        if (m_capacity == 0) {
            return true;
        }
        return (m_size + 1) * 4 > m_capacity * 3;
    }

    [[nodiscard]] usize NextCapacity() const {
        if (m_capacity == 0) {
            return kInitialCapacity;
        }
        // Saturating this the way Array<T> does (returning kMaxCapacity) is not an option:
        // kMaxCapacity is not generally a power of two, and this container's mask-based
        // indexing requires capacity to always be one. Doubling this far is unreachable in
        // practice -- AllocateEntries would already have failed long before m_capacity grew
        // anywhere near kMaxCapacity/2 -- so treating it as fatal costs nothing real.
        MONARC_CHECK(m_capacity <= kMaxCapacity / 2, "HashMap capacity would overflow usize");
        if (m_capacity > kMaxCapacity / 2) {
            OnAllocationFailed();
        }
        return m_capacity * 2;
    }

    [[nodiscard]] Entry* AllocateEntries(usize capacity) const {
        MONARC_CHECK(capacity <= kMaxCapacity, "HashMap capacity would overflow usize");
        if (capacity > kMaxCapacity) {
            OnAllocationFailed();
        }
        Entry* buffer =
            static_cast<Entry*>(m_allocator->Allocate(capacity * sizeof(Entry), alignof(Entry)));
        MONARC_CHECK(buffer != nullptr, "HashMap allocation failed");
        if (buffer == nullptr) {
            OnAllocationFailed();
        }
        return buffer;
    }

    [[nodiscard]] bool* AllocateOccupied(usize capacity) const {
        bool* buffer =
            static_cast<bool*>(m_allocator->Allocate(capacity * sizeof(bool), alignof(bool)));
        MONARC_CHECK(buffer != nullptr, "HashMap allocation failed");
        if (buffer == nullptr) {
            OnAllocationFailed();
        }
        for (usize i = 0; i < capacity; ++i) {
            buffer[i] = false;
        }
        return buffer;
    }

    /// Allocates the next table, rehashes every occupied entry into it -- moved, then
    /// destroyed at the old slot, per Array<T>::MoveElementsTo -- and adopts the result.
    void Grow() {
        const usize  newCapacity = NextCapacity();
        Entry* const newEntries  = AllocateEntries(newCapacity);
        bool* const  newOccupied = AllocateOccupied(newCapacity);
        const usize  newMask     = newCapacity - 1;

        for (usize i = 0; i < m_capacity; ++i) {
            if (!m_occupied[i]) {
                continue;
            }
            usize newIndex = static_cast<usize>(Hash(m_entries[i].key)) & newMask;
            while (newOccupied[newIndex]) {
                newIndex = (newIndex + 1) & newMask;
            }
            std::construct_at(newEntries + newIndex, std::move(m_entries[i]));
            std::destroy_at(m_entries + i);
            newOccupied[newIndex] = true;
        }

        ReleaseBuffers();
        m_entries  = newEntries;
        m_occupied = newOccupied;
        m_capacity = newCapacity;
    }

    void ReleaseBuffers() {
        if (m_entries != nullptr) {
            m_allocator->Deallocate(m_entries, m_capacity * sizeof(Entry), alignof(Entry));
            m_entries = nullptr;
        }
        if (m_occupied != nullptr) {
            m_allocator->Deallocate(m_occupied, m_capacity * sizeof(bool), alignof(bool));
            m_occupied = nullptr;
        }
        m_capacity = 0;
    }

    [[nodiscard]] usize FirstOccupiedFrom(usize start) const {
        usize index = start;
        while (index < m_capacity && !m_occupied[index]) {
            ++index;
        }
        return index;
    }

    IAllocator* m_allocator = nullptr;
    Entry*      m_entries   = nullptr;
    bool*       m_occupied  = nullptr;
    usize       m_size      = 0;
    usize       m_capacity  = 0;
};

}  // namespace Monarc

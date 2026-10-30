#pragma once

#include <cstdint>

// What a Connection knows about the channels currently open on it.
//
// Protocol state, not execution contexts. There is no task here, no queue and no
// buffer: a Connection serves exactly one operation at a time and a long upload
// holds it, which is v1's accepted head-of-line blocking. What the table buys is
// not concurrency but the ability to say something sensible about a frame that is
// not the one being served.
class ChannelTable
{
public:
    static constexpr int MAX = 6;

    enum class State : uint8_t
    {
        Free,

        // A handler is running on it: reading its request or writing its reply.
        // At most one of these exists per Connection.
        Active,

        // Open, with no handler and nothing to schedule -- the device's own logs
        // and telemetry streams. Frames go out when a drain has the opportunity;
        // nothing arrives on them.
        Passive,
    };

    struct Entry
    {
        uint16_t id = 0;
        State    state = State::Free;
    };

    Entry* Find(uint16_t id)
    {
        for (auto& e : entries_)
            if (e.state != State::Free && e.id == id) return &e;
        return nullptr;
    }

    /// Take a free slot for `id`. Null when the table is full, which the caller
    /// reports to the peer rather than dropping silently.
    ///
    /// Nothing is reclaimed here, and that is the difference from the pre-OPEN
    /// version of this class. It used to keep a Draining slot per abandoned request
    /// so residue could be recognised, which meant a peer that went away mid-body
    /// leaked one -- and the fix for the leak was to reuse draining slots, a
    /// workaround for a workaround. OPEN removes both: a frame without it, for an
    /// id nobody holds, is residue by definition and is dropped without any memory
    /// of the channel it belonged to.
    Entry* Open(uint16_t id, State state)
    {
        for (auto& e : entries_)
            if (e.state == State::Free)
            {
                e.id = id;
                e.state = state;
                return &e;
            }
        return nullptr;
    }

    void Close(uint16_t id)
    {
        if (Entry* e = Find(id)) e->state = State::Free;
    }

    /// Is a handler running? Only ever true from inside one -- at the top of a read
    /// loop this is false, because the loop is where a handler would have returned to.
    bool Busy() const
    {
        for (const auto& e : entries_)
            if (e.state == State::Active) return true;
        return false;
    }

    bool Holds(uint16_t id) const
    {
        for (const auto& e : entries_)
            if (e.state != State::Free && e.id == id) return true;
        return false;
    }

    /// A fresh transport connection knows no channels: ids from the old one mean
    /// nothing on the new one.
    void Reset()
    {
        for (auto& e : entries_) e = Entry{};
    }

private:
    Entry entries_[MAX];
};

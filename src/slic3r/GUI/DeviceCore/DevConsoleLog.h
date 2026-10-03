#pragma once

#include <cstdint>
#include <ctime>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace Slic3r {

/* Rolling record of everything exchanged with the printer.
 *
 * Recording starts with the app and does not depend on the console tab ever being opened,
 * so a fault can be inspected after it happened rather than only while reproducing it.
 * Bounded by entry count *and* total bytes, because a single MQTT push can be tens of KB.
 *
 * Writers are the network callbacks (any thread) and MachineObject::publish_*; the reader
 * is the console panel on the UI thread, hence the mutex.
 */
class DevConsoleLog
{
public:
    enum Kind {
        Push, /* received from the printer */
        Sent, /* published to the printer */
        Note, /* locally generated remark, e.g. a command result */
    };

    struct Entry
    {
        uint64_t    seq = 0;
        std::time_t when = 0;
        Kind        kind = Push;
        bool        important = false;
        std::string dev_id;
        std::string text;
    };

    /* Entries newer than `after` that are flagged important, oldest first. */
    std::vector<Entry> important_since(uint64_t after, uint64_t& out_next) const;

    static DevConsoleLog& instance();

    void add(Kind kind, const std::string& dev_id, const std::string& text);

    /* Entries newer than `after`, oldest first. `out_next` receives the seq to pass as
       `after` on the following call. */
    std::vector<Entry> since(uint64_t after, uint64_t& out_next) const;

    /* Everything currently held, oldest first. */
    std::vector<Entry> snapshot(uint64_t& out_next) const;

    void clear();

    static const char* kind_label(Kind kind);

private:
    DevConsoleLog() = default;

    void trim(); /* caller holds m_mutex */

    mutable std::mutex m_mutex;
    std::deque<Entry>  m_entries;
    size_t             m_bytes = 0;
    uint64_t           m_next_seq = 1;
};

} // namespace Slic3r

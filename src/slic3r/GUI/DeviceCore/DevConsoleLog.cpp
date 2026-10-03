#include "DevConsoleLog.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>

namespace Slic3r {

/* Hard caps. A single push on a multi-AMS machine runs to ~20 KB, so the byte cap is what
   actually bites; the count cap only matters for long runs of small frames. */
static const size_t MAX_ENTRIES    = 1500;
static const size_t MAX_TOTAL_BYTES = 4u * 1024 * 1024;
static const size_t MAX_ENTRY_BYTES = 64u * 1024;

/* Keys whose values never belong in a log the user can save and send to someone else.
   Printer telemetry does not carry account credentials, but the user topic and the LAN
   handshake do, and the console shows all three. */
static const std::array<const char*, 8> SENSITIVE_KEYS = {
    "token", "access_token", "refresh_token", "password", "passwd", "ticket", "secret", "authorization"
};

/* Replace the string value following "<key>": with a placeholder, in place.
   Deliberately string-based rather than JSON-parsed: this runs on every frame, and the
   frames are exactly the ones that are not guaranteed to be valid UTF-8. */
static void redact_key(std::string& s, const char* key)
{
    /* Both the plain form and the escaped form that appears when one JSON document is
       carried as a string inside another. */
    const std::string patterns[] = {std::string("\"") + key + "\"", std::string("\\\"") + key + "\\\""};

    for (const std::string& pat : patterns) {
        size_t pos = 0;
        while ((pos = s.find(pat, pos)) != std::string::npos) {
            size_t p = pos + pat.size();
            while (p < s.size() && (s[p] == ' ' || s[p] == '\t')) ++p;
            if (p >= s.size() || s[p] != ':') { pos = p; continue; }
            ++p;
            while (p < s.size() && (s[p] == ' ' || s[p] == '\t')) ++p;

            /* Only string values; a numeric or object value is not a credential. */
            size_t quote_len = 0;
            if (p < s.size() && s[p] == '"')
                quote_len = 1;
            else if (p + 1 < s.size() && s[p] == '\\' && s[p + 1] == '"')
                quote_len = 2;
            if (quote_len == 0) { pos = p; continue; }

            const size_t value_start = p + quote_len;
            size_t       end         = value_start;
            while (end < s.size()) {
                if (s[end] == '\\' && end + 1 < s.size()) { end += 2; continue; }
                if (s[end] == '"') break;
                ++end;
            }
            if (end >= s.size()) break;

            /* For the escaped form the closing delimiter is \" - step back over the slash. */
            size_t value_end = end;
            if (quote_len == 2 && value_end > value_start && s[value_end - 1] == '\\') --value_end;

            if (value_end > value_start) {
                s.replace(value_start, value_end - value_start, "<redacted>");
                pos = value_start + strlen("<redacted>");
            } else {
                pos = end + 1;
            }
        }
    }
}

static std::string sanitise(const std::string& in)
{
    std::string out = in.size() > MAX_ENTRY_BYTES ? in.substr(0, MAX_ENTRY_BYTES) + "\n... [truncated]" : in;
    for (const char* key : SENSITIVE_KEYS)
        redact_key(out, key);
    return out;
}

/* What the compact console shows. Deliberately substring tests rather than a JSON parse:
   this runs on every frame, several times a second, on strings up to 64 KB.

   Incremental pushes carry only the fields that changed, so the mere presence of a key like
   gcode_state means the state actually moved - which is what makes this worth filtering on
   rather than just showing everything. */
static bool classify_important(DevConsoleLog::Kind kind, const std::string& text)
{
    /* Anything we sent, and anything we remarked on, is by definition worth seeing. */
    if (kind != DevConsoleLog::Push)
        return true;

    static const char* MARKERS[] = {
        "\"command\"",      /* a reply to something we asked for */
        "\"gcode_state\"",  /* the job started, paused, finished or failed */
        "\"layer_num\"",    /* progress through the job */
        "\"print_error\"", "\"mc_print_error_code\"", "\"fail_reason\"",
        "\"hms\"",          /* health management warnings */
        "\"upgrade_state\"",
    };
    for (const char* m : MARKERS) {
        if (text.find(m) != std::string::npos)
            return true;
    }
    return false;
}

DevConsoleLog& DevConsoleLog::instance()
{
    static DevConsoleLog log;
    return log;
}

const char* DevConsoleLog::kind_label(Kind kind)
{
    switch (kind) {
    case Sent: return "TX";
    case Note: return "--";
    case Push:
    default: return "RX";
    }
}

void DevConsoleLog::add(Kind kind, const std::string& dev_id, const std::string& text)
{
    Entry e;
    e.when   = std::time(nullptr);
    e.kind   = kind;
    e.dev_id = dev_id;
    e.text   = sanitise(text);

    e.important = classify_important(kind, e.text);

    std::lock_guard<std::mutex> lock(m_mutex);
    e.seq = m_next_seq++;
    m_bytes += e.text.size();
    m_entries.push_back(std::move(e));
    trim();
}

void DevConsoleLog::trim()
{
    while (!m_entries.empty() && (m_entries.size() > MAX_ENTRIES || m_bytes > MAX_TOTAL_BYTES)) {
        m_bytes -= m_entries.front().text.size();
        m_entries.pop_front();
    }
}

std::vector<DevConsoleLog::Entry> DevConsoleLog::since(uint64_t after, uint64_t& out_next) const
{
    std::vector<Entry> result;

    std::lock_guard<std::mutex> lock(m_mutex);
    out_next = m_next_seq - 1;
    for (const Entry& e : m_entries) {
        if (e.seq > after)
            result.push_back(e);
    }
    return result;
}

std::vector<DevConsoleLog::Entry> DevConsoleLog::important_since(uint64_t after, uint64_t& out_next) const
{
    std::vector<Entry> result;

    std::lock_guard<std::mutex> lock(m_mutex);
    out_next = m_next_seq - 1;
    for (const Entry& e : m_entries) {
        if (e.seq > after && e.important)
            result.push_back(e);
    }
    return result;
}

std::vector<DevConsoleLog::Entry> DevConsoleLog::snapshot(uint64_t& out_next) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    out_next = m_next_seq - 1;
    return std::vector<Entry>(m_entries.begin(), m_entries.end());
}

void DevConsoleLog::clear()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_entries.clear();
    m_bytes = 0;
}

} // namespace Slic3r

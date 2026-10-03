#ifndef slic3r_ConsolePanel_hpp_
#define slic3r_ConsolePanel_hpp_

#include <cstdint>
#include <vector>

#include <wx/checkbox.h>
#include <wx/panel.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/timer.h>

#include "DeviceCore/DevConsoleLog.h"
#include "Widgets/Button.hpp"

namespace Slic3r {

class MachineObject;

namespace GUI {

/* View onto DevConsoleLog, with a G-code sender.
 *
 * The traffic is recorded whether or not this panel exists; the panel polls the buffer on a
 * timer. That keeps the network callbacks free of any dependency on the UI, means the log
 * survives tab switches, and lets two of these exist at once without extra plumbing.
 *
 * Two shapes:
 *   Full    - its own top-level tab. Every frame, raw or pretty-printed, with filters.
 *   Compact - embedded under the filament controls. Only entries flagged important, each
 *             boiled down to one line, because raw JSON in a box that size is useless.
 *
 * Note on the sender: Bambu firmware acknowledges `gcode_line` but does not return responses
 * the way Marlin or Klipper do, so there is nothing to echo. Readback arrives as an ordinary
 * status push, which is why sent lines and received frames share one view.
 */
class ConsolePanel : public wxPanel
{
public:
    enum class Mode { Full, Compact };

    ConsolePanel(wxWindow*      parent,
                 Mode           mode  = Mode::Full,
                 wxWindowID     id    = wxID_ANY,
                 const wxPoint& pos   = wxDefaultPosition,
                 const wxSize&  size  = wxDefaultSize,
                 long           style = wxTAB_TRAVERSAL);
    ~ConsolePanel() override;

    void msw_rescale() {}

private:
    void on_timer(wxTimerEvent& event);
    void on_send(wxCommandEvent& event);
    void on_clear(wxCommandEvent& event);
    void on_save(wxCommandEvent& event);
    void on_filter_changed(wxCommandEvent& event);
    void on_input_key(wxKeyEvent& event);
    void on_input_text(wxCommandEvent& event);
    void refresh_completions();

    void build_full();
    void build_compact();

    /* A derived, human-readable event, as shown by the compact view. */
    enum class Severity { Normal, Dim, Sent, Warning, Error };
    struct Event
    {
        wxString text;
        Severity severity = Severity::Normal;
    };

    /* Everything the compact view needs to remember in order to report only what changed.
       A status push repeats the whole state, so without this it would narrate every frame. */
    struct SeenState
    {
        bool        valid = false;
        std::string gcode_state;
        int         layer = -1;
        int         total_layers = -1;
        int         print_error = 0;
        std::string hms_codes; /* joined, so a set comparison is a string compare */
    };

    std::vector<Event> derive_events(const DevConsoleLog::Entry& entry);
    void               append_events(const std::vector<Event>& events);
    void               reset_derivation();

    void           send_current_line();
    void           drain_log();
    void           reload_all();
    bool           entry_visible(const DevConsoleLog::Entry& entry) const;
    wxString       format_entry(const DevConsoleLog::Entry& entry) const;
    MachineObject* current_machine() const;
    void           refresh_send_state();

    std::vector<DevConsoleLog::Entry> fetch_since(uint64_t after, uint64_t& next) const;

    const Mode m_mode;

    wxTextCtrl*   m_log      = nullptr;
    wxTextCtrl*   m_input    = nullptr;
    Button*       m_send_btn = nullptr;
    wxCheckBox*   m_show_rx  = nullptr;
    wxCheckBox*   m_show_tx  = nullptr;
    wxCheckBox*   m_pretty   = nullptr;
    wxCheckBox*   m_follow   = nullptr;
    wxStaticText* m_hint     = nullptr;
    wxString      m_default_hint;

    wxTimer  m_timer;
    uint64_t m_cursor = 0;
    /* -1 until the first poll, so the initial state is always applied rather than skipped
       as "unchanged". */
    int m_last_can_send = -1;

    SeenState m_seen;

    std::vector<wxString> m_history;
    int                   m_history_pos = -1;
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_ConsolePanel_hpp_

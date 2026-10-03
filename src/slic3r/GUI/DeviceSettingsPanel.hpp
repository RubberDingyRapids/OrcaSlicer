#ifndef slic3r_DeviceSettingsPanel_hpp_
#define slic3r_DeviceSettingsPanel_hpp_

#include <functional>
#include <vector>

#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/panel.h>
#include <wx/scrolwin.h>
#include <wx/stattext.h>

#include "DeviceCore/DevPrintOptions.h"

namespace Slic3r {

class MachineObject;

namespace GUI {

/* "Settings" tab on the Device page: the options that live in the printer's own firmware,
 * in one place, rather than spread across the Print Options / Safety / camera popups.
 *
 * Two rules this panel is built around:
 *
 *  - Only show what the printer says it supports. Every row is gated on a capability flag
 *    (DevPrintOptions::GetDetectionOption()->is_support_detect, or an is_support_* member).
 *    A row that is not supported is hidden, not greyed, because a greyed row invites the
 *    question of how to enable it.
 *
 *  - Never send a command the user did not ask for. Refreshing the controls from printer
 *    state happens under m_updating, so the change handlers do not fire and echo the value
 *    straight back at the printer. This matters most mid-print.
 */
class DeviceSettingsPanel : public wxScrolledWindow
{
public:
    DeviceSettingsPanel(wxWindow*      parent,
                        wxWindowID     id    = wxID_ANY,
                        const wxPoint& pos   = wxDefaultPosition,
                        const wxSize&  size  = wxDefaultSize,
                        long           style = wxTAB_TRAVERSAL);

    void update(MachineObject* obj);
    void show_status(int status);
    void msw_rescale() {}

private:
    /* One settable thing. `supported` and `read` are consulted on every refresh; `apply` is
       called only from a user action. */
    struct Row
    {
        wxPanel*      panel   = nullptr;
        wxCheckBox*   check   = nullptr;
        wxChoice*     choice  = nullptr;
        wxStaticText* caption = nullptr;

        std::function<bool(MachineObject*)> supported;
        std::function<int(MachineObject*)>  read;  /* current value, -1 if unknown */
        std::function<void(MachineObject*, int)> apply;

        bool disable_while_printing = false;
    };

    wxPanel*      add_section(const wxString& title);
    Row&          add_toggle(const wxString& label,
                             const wxString& caption,
                             std::function<bool(MachineObject*)> supported,
                             std::function<int(MachineObject*)>  read,
                             std::function<void(MachineObject*, int)> apply,
                             bool disable_while_printing = false);
    Row&          add_choice(const wxString&              label,
                             const wxString&              caption,
                             const std::vector<wxString>& options,
                             std::function<bool(MachineObject*)> supported,
                             std::function<int(MachineObject*)>  read,
                             std::function<void(MachineObject*, int)> apply,
                             bool disable_while_printing = false);

    void build();
    void refresh_rows();
    void on_row_changed(Row& row);

    /* Shorthand for the detection options, which all share one accessor shape. */
    static bool detection_supported(MachineObject* obj, PrintOptionEnum opt);
    static int  detection_value(MachineObject* obj, PrintOptionEnum opt);

    wxBoxSizer*   m_sizer    = nullptr;
    wxStaticText* m_banner   = nullptr;
    wxStaticText* m_empty    = nullptr;

    std::vector<Row*> m_rows;
    std::vector<std::pair<wxPanel*, std::vector<Row*>>> m_sections;

    MachineObject* m_obj      = nullptr;
    bool           m_updating = false;
    int            m_last_status = 0;
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_DeviceSettingsPanel_hpp_

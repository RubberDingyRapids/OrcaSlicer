#include "ConsolePanel.hpp"

#include <wx/filedlg.h>
#include <wx/textcompleter.h>
#include <wx/sizer.h>
#include <wx/wfstream.h>

#include <algorithm>
#include <string>

#include <nlohmann/json.hpp>

#include "DeviceCore/DevManager.h"
#include "DeviceManager.hpp"
#include "GUI_App.hpp"
#include "HMS.hpp"
#include "I18N.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/StateColor.hpp"

namespace Slic3r { namespace GUI {

/* Slow enough to coalesce a burst of pushes into one redraw, quick enough to feel live. */
static const int CONSOLE_POLL_MS = 250;

/* wxTextCtrl gets sluggish well before the 4 MB the log holds, so the view keeps less than
   the buffer does. Older lines stay available through Save. */
static const long CONSOLE_VIEW_MAX_CHARS = 400000;
static const long COMPACT_VIEW_MAX_CHARS = 8000;

/* Above this many unseen entries, rebuild the tail rather than appending each one. */
static const size_t BACKLOG_RELOAD_THRESHOLD = 300;

/* ---------------------------------------------------------------------------------------
 * Command completion.
 *
 * Grounded in what Orca itself sends to these printers (see the publish_gcode call sites in
 * DeviceManager.cpp, DevAxisCtrl.cpp and DevFan.cpp) plus the standard codes the firmware
 * accepts. Deliberately not a full G-code reference: the point is a short list that is known
 * to work on a Bambu machine, not every code Marlin ever defined.
 *
 * The description after ';' is a real G-code comment, so a completion can be sent verbatim
 * without harm - but send_current_line() strips it anyway so the log stays clean.
 * ------------------------------------------------------------------------------------- */
struct CommandHint
{
    const char* command;
    const char* description;
};

static const CommandHint COMMAND_HINTS[] = {
    {"G28", "home all axes"},
    {"G28 X", "home X only"},
    {"G28 Z", "home Z only"},
    {"G29", "auto bed levelling"},
    {"G90", "absolute positioning"},
    {"G91", "relative positioning"},
    {"G1 X10 F3000", "move X at feedrate"},
    {"G1 Y10 F3000", "move Y at feedrate"},
    {"G1 Z10 F900", "move Z at feedrate"},
    {"M17", "enable steppers"},
    {"M18", "release steppers"},
    {"M83", "relative extrusion"},
    {"M104 S220", "set nozzle temp, do not wait"},
    {"M109 S220", "set nozzle temp and wait"},
    {"M140 S60", "set bed temp, do not wait"},
    {"M190 S60", "set bed temp and wait"},
    {"M106 P1 S255", "part cooling fan, S is 0-255"},
    {"M106 P2 S255", "auxiliary fan"},
    {"M106 P3 S255", "chamber fan"},
    {"M107", "all fans off"},
    {"M211 S1", "soft endstops on"},
    {"M400", "wait for moves to finish"},
    {"M620 C", "AMS: close"},
    {"M620 R", "AMS: reset"},

    /* Raw MQTT commands. Anything starting with '{' is published to the device topic as-is
       rather than wrapped as G-code - that is how the camera, AMS and detection settings are
       actually controlled. The shapes come from the command_* builders in DeviceManager.cpp;
       the probes are informed guesses and the printer will say whether it accepts them. */
    {"{\"pushing\":{\"command\":\"pushall\",\"version\":1,\"push_target\":1}}", "ask for a full status push"},
    {"{\"camera\":{\"command\":\"ipcam_rtsp_set\",\"control\":\"enable\"}}", "probe: turn the local RTSP server on"},
    {"{\"camera\":{\"command\":\"ipcam_rtsp_set\",\"control\":\"disable\"}}", "probe: turn the local RTSP server off"},
    {"{\"camera\":{\"command\":\"ipcam_record_set\",\"control\":\"enable\"}}", "record while printing on"},
    {"{\"camera\":{\"command\":\"ipcam_timelapse\",\"control\":\"enable\"}}", "timelapse on"},
    {"{\"xcam\":{\"command\":\"xcam_control_set\",\"module_name\":\"allow_skip_parts\",\"control\":true}}", "probe: enable part skipping"},
    {"{\"system\":{\"command\":\"set_accessories\",\"accessory_type\":\"nozzle\"}}", "probe: accessory/nozzle info"},
    {"{\"print\":{\"command\":\"get_accessories\",\"accessory_type\":\"none\"}}", "probe: list accessories"},
};

/* The text actually sent: everything before the comment marker. */
static wxString strip_hint(const wxString& line)
{
    const int pos = line.Find(" ;");
    return pos == wxNOT_FOUND ? line : line.Left(pos).Trim();
}

static wxString hint_for(const wxString& typed)
{
    const wxString text = strip_hint(typed).Trim().Trim(false).Upper();
    if (text.empty())
        return wxString();

    /* Prefer an exact match on the bare code, e.g. "M106" out of "M106 P1 S120". */
    wxString code = text.BeforeFirst(' ');
    for (const CommandHint& h : COMMAND_HINTS) {
        const wxString candidate = wxString(h.command).Upper();
        if (candidate == text || candidate.BeforeFirst(' ') == code)
            return wxString::FromUTF8(h.description);
    }
    return wxString();
}

/* ---------------------------------------------------------------------------------------
 * Summarising, for the compact view.
 * ------------------------------------------------------------------------------------- */

static std::string json_str(const nlohmann::json& obj, const char* key)
{
    if (!obj.contains(key))
        return {};
    const nlohmann::json& v = obj[key];
    if (v.is_string())
        return v.get<std::string>();
    if (v.is_number_integer())
        return std::to_string(v.get<int64_t>());
    if (v.is_boolean())
        return v.get<bool>() ? "true" : "false";
    return {};
}

static int json_int(const nlohmann::json& obj, const char* key, int fallback)
{
    if (!obj.contains(key))
        return fallback;
    const nlohmann::json& v = obj[key];
    if (v.is_number_integer())
        return (int) v.get<int64_t>();
    if (v.is_string()) {
        try {
            return std::stoi(v.get<std::string>());
        } catch (const std::exception&) {
            return fallback;
        }
    }
    return fallback;
}

/* The firmware's gcode_state values, in the words a person would use. The previous state is
   needed to tell a fresh start from a resume. */
static wxString describe_state(const std::string& state, const std::string& previous)
{
    if (state == "RUNNING")
        return previous == "PAUSE" ? _L("Printing resumed") : _L("Printing started");
    if (state == "PAUSE")
        return _L("Paused");
    if (state == "FINISH")
        return _L("Print finished");
    if (state == "FAILED")
        return _L("Print failed");
    if (state == "PREPARE")
        return _L("Preparing");
    if (state == "SLICING")
        return _L("Slicing");
    if (state == "IDLE")
        return previous.empty() ? _L("Idle") : _L("Back to idle");
    return wxString::FromUTF8(state.c_str());
}

/* Commands worth naming when they are acknowledged. Everything else is plumbing: push_status
   in particular is the envelope every status message arrives in, and saying so on each frame
   is exactly the noise this view exists to avoid. */
static wxString describe_command(const std::string& command)
{
    if (command == "push_status" || command == "pushing" || command == "get_version")
        return wxString();
    if (command == "gcode_line")
        return _L("Command");
    if (command == "ams_change_filament")
        return _L("Filament change");
    if (command == "ams_control")
        return _L("AMS control");
    if (command == "ams_filament_setting")
        return _L("Filament settings");
    if (command == "resume")
        return _L("Resume");
    if (command == "pause")
        return _L("Pause");
    if (command == "stop")
        return _L("Stop");
    if (command == "calibration")
        return _L("Calibration");
    if (command == "project_file" || command == "gcode_file")
        return _L("Print job");
    return wxString::FromUTF8(command.c_str());
}

/* Sorted join of the active HMS codes, so "has the set changed" is one string compare. */
static std::string hms_code_set(const nlohmann::json& hms)
{
    std::vector<std::string> codes;
    for (const auto& item : hms) {
        if (!item.is_object())
            continue;
        const int attr = json_int(item, "attr", 0);
        const int code = json_int(item, "code", 0);
        if (attr != 0 || code != 0)
            codes.push_back(wxString::Format("%08X%08X", (unsigned) attr, (unsigned) code).ToStdString());
    }
    std::sort(codes.begin(), codes.end());

    std::string joined;
    for (const std::string& c : codes) {
        if (!joined.empty())
            joined += ",";
        joined += c;
    }
    return joined;
}

void ConsolePanel::reset_derivation() { m_seen = SeenState(); }

/* Turn one recorded frame into zero or more human-readable lines.
 *
 * Zero is the common case and the whole point: a status push repeats the entire state every
 * time, so a line is only produced where a value actually differs from the last one seen.
 * A quiet printer produces a quiet console. */
std::vector<ConsolePanel::Event> ConsolePanel::derive_events(const DevConsoleLog::Entry& entry)
{
    std::vector<Event> events;

    if (entry.kind == DevConsoleLog::Note) {
        events.push_back({wxString::FromUTF8(entry.text.c_str()), Severity::Warning});
        return events;
    }

    nlohmann::json root;
    try {
        root = nlohmann::json::parse(entry.text);
    } catch (const std::exception&) {
        return events; /* not JSON - the full Console tab still has it verbatim */
    }
    if (!root.contains("print") || !root["print"].is_object())
        return events;

    const nlohmann::json& p       = root["print"];
    const std::string     command = json_str(p, "command");

    if (entry.kind == DevConsoleLog::Sent) {
        if (command == "gcode_line") {
            std::string param = json_str(p, "param");
            const size_t nl   = param.find_first_of("\r\n");
            if (nl != std::string::npos)
                param = param.substr(0, nl) + " ...";
            events.push_back({_L("Sent: ") + wxString::FromUTF8(param.c_str()), Severity::Sent});
        } else {
            const wxString friendly = describe_command(command);
            if (!friendly.empty())
                events.push_back({_L("Sent: ") + friendly, Severity::Sent});
        }
        return events;
    }

    /* ---- received ---------------------------------------------------------------- */
    MachineObject* obj = current_machine();

    /* Acknowledgement of something we asked for. */
    const std::string result = json_str(p, "result");
    if (!result.empty()) {
        const wxString friendly = describe_command(command);
        if (!friendly.empty()) {
            const bool ok = result == "success";
            events.push_back({friendly + (ok ? _L(" accepted") : _L(" rejected")),
                              ok ? Severity::Normal : Severity::Error});
        }
    }

    /* Job state. */
    const std::string state = json_str(p, "gcode_state");
    if (!state.empty() && state != m_seen.gcode_state) {
        events.push_back({describe_state(state, m_seen.gcode_state), Severity::Normal});
        m_seen.gcode_state = state;
    }

    /* Layer progress. Chatty by design - one line per layer. */
    const int layer = json_int(p, "layer_num", -1);
    const int total = json_int(p, "total_layer_num", m_seen.total_layers);
    if (total > 0)
        m_seen.total_layers = total;
    if (layer >= 0 && layer != m_seen.layer) {
        m_seen.layer = layer;
        if (m_seen.total_layers > 0)
            events.push_back({wxString::Format(_L("Layer %d of %d"), layer, m_seen.total_layers), Severity::Dim});
        else
            events.push_back({wxString::Format(_L("Layer %d"), layer), Severity::Dim});
    }

    /* Print errors, with the text the error dialog would show rather than the code. */
    if (p.contains("print_error")) {
        const int err = json_int(p, "print_error", 0);
        if (err != m_seen.print_error) {
            if (err == 0) {
                events.push_back({_L("Error cleared"), Severity::Normal});
            } else {
                wxString msg;
                if (obj != nullptr && wxGetApp().get_hms_query() != nullptr)
                    msg = wxGetApp().get_hms_query()->query_print_error_msg(obj, err);
                if (msg.empty())
                    msg = wxString::Format("0x%08X", (unsigned) err);
                events.push_back({_L("Error: ") + msg, Severity::Error});
            }
            m_seen.print_error = err;
        }
    }

    /* HMS warnings: report the ones that appeared, and say when they all go away. */
    if (p.contains("hms") && p["hms"].is_array()) {
        const std::string codes = hms_code_set(p["hms"]);
        if (codes != m_seen.hms_codes) {
            if (codes.empty()) {
                events.push_back({_L("All warnings cleared"), Severity::Normal});
            } else {
                for (const auto& item : p["hms"]) {
                    if (!item.is_object())
                        continue;
                    const unsigned attr = (unsigned) json_int(item, "attr", 0);
                    const unsigned code = (unsigned) json_int(item, "code", 0);
                    if (attr == 0 && code == 0)
                        continue;
                    const std::string long_code = wxString::Format("%08X%08X", attr, code).ToStdString();
                    /* Only the ones that were not already active. */
                    if (m_seen.hms_codes.find(long_code) != std::string::npos)
                        continue;
                    wxString msg;
                    if (obj != nullptr && wxGetApp().get_hms_query() != nullptr)
                        msg = wxGetApp().get_hms_query()->query_hms_msg(obj, long_code);
                    if (msg.empty())
                        msg = wxString::FromUTF8(long_code.c_str());
                    events.push_back({_L("Warning: ") + msg, Severity::Warning});
                }
            }
            m_seen.hms_codes = codes;
        }
    }

    m_seen.valid = true;
    return events;
}

void ConsolePanel::append_events(const std::vector<Event>& events)
{
    if (events.empty())
        return;

    /* Severity colours have to be picked per theme: the "normal" colour is near-black on
       light and near-white on dark, and the accents need lifting to stay readable. */
    const bool dark = wxGetApp().dark_mode();

    m_log->Freeze();
    for (const Event& e : events) {
        wxColour colour;
        switch (e.severity) {
        case Severity::Error:   colour = dark ? wxColour(0xFF, 0x6B, 0x6B) : wxColour(0xC0, 0x28, 0x28); break;
        case Severity::Warning: colour = dark ? wxColour(0xF0, 0xB4, 0x4E) : wxColour(0xC5, 0x80, 0x1A); break;
        case Severity::Sent:    colour = dark ? wxColour(0x6A, 0xB8, 0xF0) : wxColour(0x1B, 0x6B, 0xA8); break;
        case Severity::Dim:     colour = dark ? wxColour(0x8A, 0x8A, 0x8A) : wxColour(0x90, 0x90, 0x90); break;
        case Severity::Normal:
        default:                colour = dark ? wxColour(0xE0, 0xE0, 0xE0) : wxColour(0x32, 0x3A, 0x3D); break;
        }
        m_log->SetDefaultStyle(wxTextAttr(colour));
        m_log->AppendText(e.text + "\n");
    }
    if (m_log->GetLastPosition() > COMPACT_VIEW_MAX_CHARS)
        m_log->Remove(0, m_log->GetLastPosition() - COMPACT_VIEW_MAX_CHARS);
    m_log->ShowPosition(m_log->GetLastPosition());
    m_log->Thaw();
}

/* ---------------------------------------------------------------------------------------
 * Panel
 * ------------------------------------------------------------------------------------- */

ConsolePanel::ConsolePanel(wxWindow* parent, Mode mode, wxWindowID id, const wxPoint& pos, const wxSize& size, long style)
    : wxPanel(parent, id, pos, size, style), m_mode(mode)
{
    /* The compact view lives inside a themed StaticBox, so it takes its parent's background
       rather than forcing one; hardcoding white shows through as a pale block in dark mode. */
    if (mode == Mode::Compact)
        SetBackgroundColour(parent->GetBackgroundColour());
    else
        SetBackgroundColour(StateColor::darkModeColorFor(*wxWHITE));

    if (m_mode == Mode::Full)
        build_full();
    else
        build_compact();

    m_send_btn->Bind(wxEVT_BUTTON, &ConsolePanel::on_send, this);
    m_input->Bind(wxEVT_TEXT_ENTER, &ConsolePanel::on_send, this);
    m_input->Bind(wxEVT_KEY_DOWN, &ConsolePanel::on_input_key, this);
    m_input->Bind(wxEVT_TEXT, &ConsolePanel::on_input_text, this);

    refresh_completions();

    reload_all();
    refresh_send_state();

    m_timer.SetOwner(this);
    Bind(wxEVT_TIMER, &ConsolePanel::on_timer, this);
    m_timer.Start(CONSOLE_POLL_MS);
}

ConsolePanel::~ConsolePanel() { m_timer.Stop(); }

void ConsolePanel::build_full()
{
    wxBoxSizer* root = new wxBoxSizer(wxVERTICAL);

    wxBoxSizer* filter_sizer = new wxBoxSizer(wxHORIZONTAL);

    m_show_rx = new wxCheckBox(this, wxID_ANY, _L("Received"));
    m_show_tx = new wxCheckBox(this, wxID_ANY, _L("Sent"));
    m_pretty  = new wxCheckBox(this, wxID_ANY, _L("Pretty print"));
    m_follow  = new wxCheckBox(this, wxID_ANY, _L("Follow output"));
    m_show_rx->SetValue(true);
    m_show_tx->SetValue(true);
    m_follow->SetValue(true);
    m_pretty->SetToolTip(_L("Indent each message over several lines. Easier to read, much longer."));
    m_follow->SetToolTip(_L("Keep scrolling to the newest message. Untick to read back - recording continues."));

    Button* clear_btn = new Button(this, _L("Clear"));
    clear_btn->SetStyle(ButtonStyle::Regular, ButtonType::Window);
    Button* save_btn = new Button(this, _L("Save to file"));
    save_btn->SetStyle(ButtonStyle::Regular, ButtonType::Window);
    clear_btn->Bind(wxEVT_BUTTON, &ConsolePanel::on_clear, this);
    save_btn->Bind(wxEVT_BUTTON, &ConsolePanel::on_save, this);

    filter_sizer->Add(m_show_rx, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(10));
    filter_sizer->Add(m_show_tx, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(12));
    filter_sizer->Add(m_pretty, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(12));
    filter_sizer->Add(m_follow, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(12));
    filter_sizer->AddStretchSpacer();
    filter_sizer->Add(save_btn, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
    filter_sizer->Add(clear_btn, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(10));

    root->Add(filter_sizer, 0, wxEXPAND | wxTOP | wxBOTTOM, FromDIP(8));

    m_log = new wxTextCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize,
                           wxTE_MULTILINE | wxTE_READONLY | wxTE_DONTWRAP | wxTE_RICH2);
    m_log->SetFont(wxFont(wxFontInfo(Label::Body_12.GetPointSize() - 1).Family(wxFONTFAMILY_TELETYPE)));
    root->Add(m_log, 1, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(10));

    wxBoxSizer* send_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_input = new wxTextCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxTE_PROCESS_ENTER);
    m_input->SetHint(_L("G-code, e.g. G28 or M106 S128 - Enter to send, Up/Down for history"));
    m_send_btn = new Button(this, _L("Send"));
    m_send_btn->SetStyle(ButtonStyle::Confirm, ButtonType::Window);

    send_sizer->Add(m_input, 1, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(10));
    send_sizer->Add(m_send_btn, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, FromDIP(8));
    root->Add(send_sizer, 0, wxEXPAND | wxTOP, FromDIP(8));

    m_hint = new wxStaticText(this, wxID_ANY, wxEmptyString);
    m_hint->SetForegroundColour(wxColour(0x6B, 0x6B, 0x6B));
    m_hint->SetFont(Label::Body_12);
    root->Add(m_hint, 0, wxEXPAND | wxALL, FromDIP(10));

    m_show_rx->Bind(wxEVT_CHECKBOX, &ConsolePanel::on_filter_changed, this);
    m_show_tx->Bind(wxEVT_CHECKBOX, &ConsolePanel::on_filter_changed, this);
    m_pretty->Bind(wxEVT_CHECKBOX, &ConsolePanel::on_filter_changed, this);

    SetSizer(root);
    Layout();
}

void ConsolePanel::build_compact()
{
    wxBoxSizer* root = new wxBoxSizer(wxVERTICAL);

    m_log = new wxTextCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize,
                           wxTE_MULTILINE | wxTE_READONLY | wxTE_DONTWRAP | wxTE_RICH2 | wxBORDER_NONE);
    m_log->SetFont(wxFont(wxFontInfo(Label::Body_12.GetPointSize() - 1).Family(wxFONTFAMILY_TELETYPE)));
    m_log->SetBackgroundColour(StateColor::darkModeColorFor(wxColour(0xF4, 0xF4, 0xF4)));
    m_log->SetMinSize(wxSize(-1, FromDIP(150)));
    root->Add(m_log, 1, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(10));

    wxBoxSizer* send_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_input = new wxTextCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxTE_PROCESS_ENTER);
    m_input->SetHint(_L("Send G-code - Enter to send, Up/Down for history"));
    m_send_btn = new Button(this, _L("Send"));
    m_send_btn->SetStyle(ButtonStyle::Confirm, ButtonType::Compact);

    send_sizer->Add(m_input, 1, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(10));
    send_sizer->Add(m_send_btn, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, FromDIP(6));
    root->Add(send_sizer, 0, wxEXPAND | wxTOP | wxBOTTOM, FromDIP(6));

    SetSizer(root);
    Layout();
}

/* ------------------------------------------------------------------------------------- */

MachineObject* ConsolePanel::current_machine() const
{
    DeviceManager* dm = wxGetApp().getDeviceManager();
    return dm ? dm->get_selected_machine() : nullptr;
}

void ConsolePanel::refresh_send_state()
{
    MachineObject* obj      = current_machine();
    const int      can_send = (obj != nullptr && obj->is_connected()) ? 1 : 0;
    if (can_send == m_last_can_send)
        return;
    m_last_can_send = can_send;

    m_send_btn->Enable(can_send);
    m_input->Enable(can_send);

    if (m_hint == nullptr)
        return;

    /* Kept so the typing hint can put it back when the box is cleared. */
    if (can_send) {
        m_default_hint = _L("Start typing for suggestions. Commands are acknowledged but not echoed - "
                            "the printer answers with a status push, shown above as a received message.");
    } else {
        m_default_hint = _L("Connect to a printer to send commands. Recording continues regardless.");
    }
    m_hint->SetLabel(m_default_hint);
    m_hint->Wrap(std::max(FromDIP(300), GetSize().GetWidth() - FromDIP(30)));
    Layout();
}

bool ConsolePanel::entry_visible(const DevConsoleLog::Entry& entry) const
{
    if (m_mode == Mode::Compact)
        return entry.important;

    switch (entry.kind) {
    case DevConsoleLog::Push: return m_show_rx->GetValue();
    case DevConsoleLog::Sent: return m_show_tx->GetValue();
    case DevConsoleLog::Note:
    default: return true;
    }
}

wxString ConsolePanel::format_entry(const DevConsoleLog::Entry& entry) const
{
    /* Full view only; the compact view renders derived events, not raw frames. */
    wxDateTime when((time_t) entry.when);

    wxString body = wxString::FromUTF8(entry.text.c_str());
    if (body.empty() && !entry.text.empty()) /* not valid UTF-8 - show it rather than dropping it */
        body = wxString::From8BitData(entry.text.data(), entry.text.size());

    if (m_pretty != nullptr && m_pretty->GetValue()) {
        try {
            const nlohmann::json parsed = nlohmann::json::parse(entry.text);
            /* Same hazard as the bridge: printer data is not guaranteed valid UTF-8, and a
               bare dump() throws type_error.316 on a stray byte. */
            body = wxString::FromUTF8(parsed.dump(2, ' ', false, nlohmann::json::error_handler_t::replace).c_str());
        } catch (const std::exception&) {
            /* not JSON, or unparseable - fall through with the raw text */
        }
    }

    return wxString::Format("[%s] %s  %s\n", when.Format("%H:%M:%S"), DevConsoleLog::kind_label(entry.kind), body);
}

std::vector<DevConsoleLog::Entry> ConsolePanel::fetch_since(uint64_t after, uint64_t& next) const
{
    return m_mode == Mode::Compact ? DevConsoleLog::instance().important_since(after, next)
                                   : DevConsoleLog::instance().since(after, next);
}

void ConsolePanel::drain_log()
{
    uint64_t                                next    = m_cursor;
    const std::vector<DevConsoleLog::Entry> entries = fetch_since(m_cursor, next);
    if (entries.empty()) {
        m_cursor = next;
        return;
    }

    /* A long spell out of sight, or with following paused, leaves a backlog bigger than the
       view can hold. Rebuilding the tail is cheaper than formatting all of it. */
    if (entries.size() > BACKLOG_RELOAD_THRESHOLD) {
        reload_all();
        return;
    }

    if (m_mode == Mode::Compact) {
        std::vector<Event> events;
        for (const DevConsoleLog::Entry& e : entries) {
            const std::vector<Event> derived = derive_events(e);
            events.insert(events.end(), derived.begin(), derived.end());
        }
        m_cursor = next;
        append_events(events);
        return;
    }

    const long max_chars = CONSOLE_VIEW_MAX_CHARS;

    wxString chunk;
    for (const DevConsoleLog::Entry& e : entries) {
        if (entry_visible(e))
            chunk += format_entry(e);
    }
    m_cursor = next;

    if (chunk.empty())
        return;

    m_log->Freeze();
    m_log->AppendText(chunk);
    if (m_log->GetLastPosition() > max_chars)
        m_log->Remove(0, m_log->GetLastPosition() - max_chars);
    m_log->ShowPosition(m_log->GetLastPosition());
    m_log->Thaw();
}

void ConsolePanel::reload_all()
{
    uint64_t                                next    = 0;
    const std::vector<DevConsoleLog::Entry> entries = DevConsoleLog::instance().snapshot(next);
    m_cursor                                        = next;

    if (m_mode == Mode::Compact) {
        /* Replay from a bounded tail rather than the whole buffer: derivation has to run in
           order to know what changed, and the view holds far less than the log does. The
           first frame of the replay reports the current state, which is what you want on
           opening the page anyway. */
        reset_derivation();
        m_log->Clear();

        const size_t REPLAY = 250;
        const size_t start  = entries.size() > REPLAY ? entries.size() - REPLAY : 0;

        std::vector<Event> events;
        for (size_t i = start; i < entries.size(); ++i) {
            if (!entries[i].important)
                continue;
            const std::vector<Event> derived = derive_events(entries[i]);
            events.insert(events.end(), derived.begin(), derived.end());
        }
        append_events(events);
        return;
    }

    const long max_chars = CONSOLE_VIEW_MAX_CHARS;

    /* Walk backwards and stop at the view limit. Formatting the whole 4 MB buffer and then
       trimming it would mean pretty-printing megabytes to throw nearly all of it away, and
       wxTextCtrl is slow enough to notice. */
    std::vector<wxString> tail;
    size_t                chars = 0;
    for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
        if (!entry_visible(*it))
            continue;
        wxString line = format_entry(*it);
        if (line.empty())
            continue;
        chars += line.length();
        tail.push_back(std::move(line));
        if (chars >= (size_t) max_chars)
            break;
    }

    wxString all;
    all.reserve(chars);
    for (auto it = tail.rbegin(); it != tail.rend(); ++it)
        all += *it;

    m_log->Freeze();
    m_log->SetValue(all);
    m_log->ShowPosition(m_log->GetLastPosition());
    m_log->Thaw();
}

void ConsolePanel::on_timer(wxTimerEvent& event)
{
    if (!IsShownOnScreen())
        return; /* leave the cursor alone; catch up when the panel comes back */

    refresh_send_state();

    if (m_follow != nullptr && !m_follow->GetValue())
        return; /* paused for reading - the log keeps recording */

    drain_log();
}

void ConsolePanel::on_filter_changed(wxCommandEvent& event) { reload_all(); }

void ConsolePanel::on_clear(wxCommandEvent& event)
{
    DevConsoleLog::instance().clear();
    reload_all();
}

void ConsolePanel::on_save(wxCommandEvent& event)
{
    wxFileDialog dlg(this, _L("Save console log"), wxEmptyString,
                     wxString::Format("printer-console-%s.log", wxDateTime::Now().Format("%Y%m%d-%H%M%S")),
                     "Log files (*.log)|*.log|All files|*.*", wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
    if (dlg.ShowModal() != wxID_OK)
        return;

    uint64_t                                next    = 0;
    const std::vector<DevConsoleLog::Entry> entries = DevConsoleLog::instance().snapshot(next);

    wxFileOutputStream out(dlg.GetPath());
    if (!out.IsOk()) {
        DevConsoleLog::instance().add(DevConsoleLog::Note, "", "could not open the file for writing");
        return;
    }
    for (const DevConsoleLog::Entry& e : entries) {
        const wxScopedCharBuffer line = format_entry(e).ToUTF8();
        out.Write(line.data(), line.length());
    }
}

/* wxTextEntry's plain AutoComplete(wxArrayString) only matches from the start of the string,
   which is useless for the raw MQTT entries - they all begin with '{', so you would have to
   know the JSON before you could be offered it. A completer matches anywhere in the line. */
class ConsoleCompleter : public wxTextCompleterSimple
{
public:
    explicit ConsoleCompleter(std::vector<wxString> items) : m_items(std::move(items)) {}

    void GetCompletions(const wxString& prefix, wxArrayString& res) override
    {
        const wxString needle = prefix.Lower();
        if (needle.empty())
            return;
        for (const wxString& item : m_items) {
            if (item.Lower().Contains(needle))
                res.Add(item);
        }
    }

private:
    std::vector<wxString> m_items;
};

/* Rebuilt after each send so previously used lines are offered back. */
void ConsolePanel::refresh_completions()
{
    std::vector<wxString> items;
    for (const CommandHint& h : COMMAND_HINTS)
        items.push_back(wxString::Format("%s ; %s", h.command, h.description));
    for (const wxString& past : m_history) {
        if (std::find(items.begin(), items.end(), past) == items.end())
            items.push_back(past);
    }
    /* wxTextEntry takes ownership of the completer. */
    m_input->AutoComplete(new ConsoleCompleter(std::move(items)));
}

/* Describe what is being typed, where there is room to say it. */
void ConsolePanel::on_input_text(wxCommandEvent& event)
{
    event.Skip();
    if (m_hint == nullptr)
        return;

    const wxString desc = hint_for(m_input->GetValue());
    const wxString text = desc.empty() ? m_default_hint : strip_hint(m_input->GetValue()) + "  -  " + desc;
    if (text == m_hint->GetLabel())
        return;
    m_hint->SetLabel(text);
    m_hint->Wrap(std::max(FromDIP(300), GetSize().GetWidth() - FromDIP(30)));
    Layout();
}

void ConsolePanel::on_send(wxCommandEvent& event) { send_current_line(); }

void ConsolePanel::send_current_line()
{
    /* A completion carries its description as a G-code comment; the printer would ignore it
       but there is no reason to send or log it. */
    const wxString line = strip_hint(m_input->GetValue()).Trim().Trim(false);
    if (line.empty())
        return;

    MachineObject* obj = current_machine();
    if (obj == nullptr || !obj->is_connected()) {
        DevConsoleLog::instance().add(DevConsoleLog::Note, "", "not connected - command not sent");
        drain_log();
        return;
    }

    int ret = 0;

    if (line.StartsWith("{")) {
        /* Raw MQTT command. Most of what a Bambu printer can be told to do is not G-code at
           all - camera, AMS, detection and firmware settings all arrive as JSON on the
           device topic - and almost none of it has a UI. Sending it from here is the only
           way to find out what the firmware actually accepts without a rebuild per guess. */
        nlohmann::json payload;
        try {
            payload = nlohmann::json::parse(std::string(line.ToUTF8().data()));
        } catch (const std::exception& e) {
            DevConsoleLog::instance().add(DevConsoleLog::Note, "", std::string("not valid JSON: ") + e.what());
            drain_log();
            return;
        }
        if (!payload.is_object() || payload.empty()) {
            DevConsoleLog::instance().add(DevConsoleLog::Note, "", "JSON must be an object, e.g. {\"pushing\":{...}}");
            drain_log();
            return;
        }

        /* The printer correlates replies by sequence_id; fill one in when it is missing so
           the acknowledgement can be matched to what was sent. */
        for (auto& section : payload.items()) {
            if (section.value().is_object() && !section.value().contains("sequence_id"))
                section.value()["sequence_id"] = std::to_string(MachineObject::m_sequence_id++);
        }

        ret = obj->publish_json(payload);
    } else {
        ret = obj->publish_gcode(std::string(line.ToUTF8().data()) + "\n");
    }

    if (ret != 0) {
        DevConsoleLog::instance().add(DevConsoleLog::Note, obj->get_dev_id(),
                                      "publish failed, code " + std::to_string(ret));
    }

    m_history.push_back(line);
    m_history_pos = (int) m_history.size();
    m_input->Clear();
    refresh_completions();
    drain_log();
}

void ConsolePanel::on_input_key(wxKeyEvent& event)
{
    if (m_history.empty()) {
        event.Skip();
        return;
    }

    if (event.GetKeyCode() == WXK_UP) {
        if (m_history_pos > 0) {
            --m_history_pos;
            m_input->SetValue(m_history[m_history_pos]);
            m_input->SetInsertionPointEnd();
        }
    } else if (event.GetKeyCode() == WXK_DOWN) {
        if (m_history_pos < (int) m_history.size() - 1) {
            ++m_history_pos;
            m_input->SetValue(m_history[m_history_pos]);
            m_input->SetInsertionPointEnd();
        } else {
            m_history_pos = (int) m_history.size();
            m_input->Clear();
        }
    } else {
        event.Skip();
    }
}

}} // namespace Slic3r::GUI

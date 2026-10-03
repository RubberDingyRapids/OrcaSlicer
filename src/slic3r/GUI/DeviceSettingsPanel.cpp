#include "DeviceSettingsPanel.hpp"

#include <algorithm>

#include <wx/sizer.h>

#include "DeviceCore/DevFilaSystem.h"
#include "DeviceManager.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/SideTools.hpp"

namespace Slic3r { namespace GUI {

static wxColour SECTION_TITLE_COL = wxColour(0x32, 0x3A, 0x3D);
static wxColour CAPTION_COL       = wxColour(0x6B, 0x6B, 0x6B);
static wxColour BANNER_COL        = wxColour(0xC5, 0x80, 0x1A);

/* Sensitivity levels, in the order the firmware names them. Index maps to the string the
   xcam commands expect. */
static const char* SENSITIVITY_KEYS[] = {"never", "low", "medium", "high"};

/* Both of these are owned by MachineObject but constructed lazily, so a row can be asked to
   read itself before they exist. */
static DevPrintOptions* opts_of(MachineObject* o) { return o != nullptr ? o->GetPrintOptions() : nullptr; }

static DevAmsSystemSetting* ams_setting_of(MachineObject* o)
{
    if (o == nullptr)
        return nullptr;
    const std::shared_ptr<DevFilaSystem> fila = o->GetFilaSystem();
    return fila ? &fila->GetAmsSystemSetting() : nullptr;
}

static int sensitivity_index(const std::string& key)
{
    for (int i = 0; i < (int) (sizeof(SENSITIVITY_KEYS) / sizeof(SENSITIVITY_KEYS[0])); ++i) {
        if (key == SENSITIVITY_KEYS[i])
            return i;
    }
    return 2; /* medium - what the firmware defaults to */
}

DeviceSettingsPanel::DeviceSettingsPanel(wxWindow* parent, wxWindowID id, const wxPoint& pos, const wxSize& size, long style)
    : wxScrolledWindow(parent, id, pos, size, style)
{
    SetBackgroundColour(*wxWHITE);
    SetScrollRate(0, FromDIP(10));
    build();
}

bool DeviceSettingsPanel::detection_supported(MachineObject* obj, PrintOptionEnum opt)
{
    if (obj == nullptr || obj->GetPrintOptions() == nullptr)
        return false;
    const PrintOptionData* data = obj->GetPrintOptions()->GetDetectionOption(opt);
    return data != nullptr && data->is_support_detect;
}

int DeviceSettingsPanel::detection_value(MachineObject* obj, PrintOptionEnum opt)
{
    if (obj == nullptr || obj->GetPrintOptions() == nullptr)
        return -1;
    const PrintOptionData* data = obj->GetPrintOptions()->GetDetectionOption(opt);
    return data != nullptr ? data->current_detect_value : -1;
}

wxPanel* DeviceSettingsPanel::add_section(const wxString& title)
{
    wxPanel*    panel = new wxPanel(this);
    wxBoxSizer* inner = new wxBoxSizer(wxVERTICAL);
    panel->SetBackgroundColour(*wxWHITE);

    Label* label = new Label(panel, title);
    label->SetFont(Label::Head_14);
    label->SetForegroundColour(SECTION_TITLE_COL);
    inner->Add(label, 0, wxTOP | wxBOTTOM, FromDIP(8));

    panel->SetSizer(inner);
    m_sizer->Add(panel, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(18));

    m_sections.emplace_back(panel, std::vector<Row*>{});
    return panel;
}

DeviceSettingsPanel::Row& DeviceSettingsPanel::add_toggle(const wxString& label,
                                                          const wxString& caption,
                                                          std::function<bool(MachineObject*)> supported,
                                                          std::function<int(MachineObject*)>  read,
                                                          std::function<void(MachineObject*, int)> apply,
                                                          bool disable_while_printing)
{
    wxPanel*    host  = m_sections.back().first;
    wxBoxSizer* inner = static_cast<wxBoxSizer*>(host->GetSizer());

    Row* row = new Row();
    row->panel = new wxPanel(host);
    row->panel->SetBackgroundColour(*wxWHITE);
    wxBoxSizer* row_sizer = new wxBoxSizer(wxVERTICAL);

    row->check = new wxCheckBox(row->panel, wxID_ANY, label);
    row_sizer->Add(row->check, 0, wxTOP, FromDIP(4));

    if (!caption.empty()) {
        row->caption = new wxStaticText(row->panel, wxID_ANY, caption);
        row->caption->SetForegroundColour(CAPTION_COL);
        row->caption->SetFont(Label::Body_12);
        row_sizer->Add(row->caption, 0, wxLEFT | wxBOTTOM, FromDIP(22));
    }

    row->panel->SetSizer(row_sizer);
    inner->Add(row->panel, 0, wxEXPAND);

    row->supported              = std::move(supported);
    row->read                   = std::move(read);
    row->apply                  = std::move(apply);
    row->disable_while_printing = disable_while_printing;

    row->check->Bind(wxEVT_CHECKBOX, [this, row](wxCommandEvent& e) {
        e.Skip();
        on_row_changed(*row);
    });

    m_rows.push_back(row);
    m_sections.back().second.push_back(row);
    return *row;
}

DeviceSettingsPanel::Row& DeviceSettingsPanel::add_choice(const wxString&              label,
                                                          const wxString&              caption,
                                                          const std::vector<wxString>& options,
                                                          std::function<bool(MachineObject*)> supported,
                                                          std::function<int(MachineObject*)>  read,
                                                          std::function<void(MachineObject*, int)> apply,
                                                          bool disable_while_printing)
{
    wxPanel*    host  = m_sections.back().first;
    wxBoxSizer* inner = static_cast<wxBoxSizer*>(host->GetSizer());

    Row* row = new Row();
    row->panel = new wxPanel(host);
    row->panel->SetBackgroundColour(*wxWHITE);
    wxBoxSizer* row_sizer = new wxBoxSizer(wxVERTICAL);

    wxBoxSizer* line = new wxBoxSizer(wxHORIZONTAL);
    wxStaticText* text = new wxStaticText(row->panel, wxID_ANY, label);
    line->Add(text, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(10));

    wxArrayString choices;
    for (const wxString& o : options)
        choices.Add(o);
    row->choice = new wxChoice(row->panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, choices);
    line->Add(row->choice, 0, wxALIGN_CENTER_VERTICAL);

    row_sizer->Add(line, 0, wxTOP, FromDIP(4));

    if (!caption.empty()) {
        row->caption = new wxStaticText(row->panel, wxID_ANY, caption);
        row->caption->SetForegroundColour(CAPTION_COL);
        row->caption->SetFont(Label::Body_12);
        row_sizer->Add(row->caption, 0, wxBOTTOM, FromDIP(4));
    }

    row->panel->SetSizer(row_sizer);
    inner->Add(row->panel, 0, wxEXPAND);

    row->supported              = std::move(supported);
    row->read                   = std::move(read);
    row->apply                  = std::move(apply);
    row->disable_while_printing = disable_while_printing;

    row->choice->Bind(wxEVT_CHOICE, [this, row](wxCommandEvent& e) {
        e.Skip();
        on_row_changed(*row);
    });

    m_rows.push_back(row);
    m_sections.back().second.push_back(row);
    return *row;
}

void DeviceSettingsPanel::on_row_changed(Row& row)
{
    if (m_updating || m_obj == nullptr || !row.apply)
        return;

    const int value = row.check != nullptr ? (row.check->GetValue() ? 1 : 0) : row.choice->GetSelection();
    if (value < 0)
        return;

    row.apply(m_obj, value);
}

void DeviceSettingsPanel::build()
{
    m_sizer = new wxBoxSizer(wxVERTICAL);

    m_banner = new wxStaticText(this, wxID_ANY, wxEmptyString);
    m_banner->SetForegroundColour(BANNER_COL);
    m_banner->SetFont(Label::Body_13);
    m_sizer->Add(m_banner, 0, wxEXPAND | wxALL, FromDIP(18));
    m_banner->Hide();

    m_empty = new wxStaticText(this, wxID_ANY,
                               _L("Connect to a printer to see the settings it supports."));
    m_empty->SetForegroundColour(CAPTION_COL);
    m_sizer->Add(m_empty, 0, wxEXPAND | wxALL, FromDIP(18));

    /* ---- Printing ------------------------------------------------------------------ */
    add_section(_L("Printing"));

    add_choice(_L("Print speed"), _L("Applies immediately, including to a job already running."),
               {_L("Silent"), _L("Standard"), _L("Sport"), _L("Ludicrous")},
               [](MachineObject* o) { return o->GetPrintingSpeedLevel() != SPEED_LEVEL_INVALID; },
               [](MachineObject* o) {
                   const int lvl = (int) o->GetPrintingSpeedLevel();
                   return lvl >= SPEED_LEVEL_SILENCE ? lvl - 1 : -1;
               },
               [](MachineObject* o, int v) { o->command_set_printing_speed((DevPrintingSpeedLevel) (v + 1)); });

    add_toggle(_L("Auto-recover from step loss"),
               _L("Resume automatically if the printer detects the toolhead has skipped steps."),
               [](MachineObject* o) { return o->is_support_auto_recovery_step_loss && opts_of(o) != nullptr; },
               [](MachineObject* o) { return detection_value(o, PrintOptionEnum::Auto_Recovery_Detection); },
               [](MachineObject* o, int v) { opts_of(o)->command_xcam_control_auto_recovery_step_loss(v != 0); });

    add_toggle(_L("Filament tangle detection"),
               _L("Pause if the filament is detected as tangled on the spool."),
               [](MachineObject* o) { return o->is_support_filament_tangle_detect && opts_of(o) != nullptr; },
               [](MachineObject* o) { return detection_value(o, PrintOptionEnum::Filament_Tangle_Detection); },
               [](MachineObject* o, int v) { opts_of(o)->command_xcam_control_filament_tangle_detect(v != 0); });

    add_toggle(_L("Idle heating protection"),
               _L("Lower the nozzle temperature when the printer is left hot and idle."),
               [](MachineObject* o) { return detection_supported(o, PrintOptionEnum::Idle_Heating_Protect_Detection); },
               [](MachineObject* o) { return detection_value(o, PrintOptionEnum::Idle_Heating_Protect_Detection); },
               [](MachineObject* o, int v) { opts_of(o)->command_xcam_control_idelheatingprotect_detector(v != 0); });

    add_toggle(_L("Prompt sound"), _L("The printer's buzzer for job start, finish and warnings."),
               [](MachineObject* o) { return o->is_support_prompt_sound && opts_of(o) != nullptr; },
               [](MachineObject* o) { return detection_value(o, PrintOptionEnum::Allow_Prompt_Sound_Detection); },
               [](MachineObject* o, int v) { opts_of(o)->command_xcam_control_allow_prompt_sound(v != 0); });

    /* ---- Detection ----------------------------------------------------------------- */
    add_section(_L("Detection"));

    add_toggle(_L("AI monitoring of printing"),
               _L("Watch the print for failures and pause if one is detected."),
               [](MachineObject* o) { return detection_supported(o, PrintOptionEnum::AI_Monitoring); },
               [](MachineObject* o) { return detection_value(o, PrintOptionEnum::AI_Monitoring); },
               [](MachineObject* o, int v) {
                   opts_of(o)->command_xcam_control_ai_monitoring(v != 0, opts_of(o)->GetAiMonitoringSensitivity());
               });

    add_choice(_L("Pausing sensitivity"), wxEmptyString,
               {_L("Never"), _L("Low"), _L("Medium"), _L("High")},
               [](MachineObject* o) { return detection_supported(o, PrintOptionEnum::AI_Monitoring); },
               [](MachineObject* o) { return sensitivity_index(opts_of(o)->GetAiMonitoringSensitivity()); },
               [](MachineObject* o, int v) {
                   opts_of(o)->command_xcam_control_ai_monitoring(opts_of(o)->GetAiMonitoring(), SENSITIVITY_KEYS[v]);
               });

    add_toggle(_L("First layer inspection"), _L("Check the first layer before carrying on with the job."),
               [](MachineObject* o) { return detection_supported(o, PrintOptionEnum::First_Layer_Detection); },
               [](MachineObject* o) { return detection_value(o, PrintOptionEnum::First_Layer_Detection); },
               [](MachineObject* o, int v) { opts_of(o)->command_xcam_control_first_layer_inspector(v != 0, false); });

    add_toggle(_L("Build plate detection"), _L("Check the plate type and position before starting."),
               [](MachineObject* o) { return detection_supported(o, PrintOptionEnum::Buildplate_Mark_Detection); },
               [](MachineObject* o) { return detection_value(o, PrintOptionEnum::Buildplate_Mark_Detection); },
               [](MachineObject* o, int v) { opts_of(o)->command_xcam_control_buildplate_marker_detector(v != 0); });

    add_toggle(_L("Build plate alignment detection"), wxEmptyString,
               [](MachineObject* o) { return detection_supported(o, PrintOptionEnum::Buildplate_Align_Detection); },
               [](MachineObject* o) { return detection_value(o, PrintOptionEnum::Buildplate_Align_Detection); },
               [](MachineObject* o, int v) { opts_of(o)->command_xcam_control_build_plate_align_detector(v != 0); });

    add_toggle(_L("Foreign object detection"), _L("Check the plate is clear before starting a job."),
               [](MachineObject* o) { return detection_supported(o, PrintOptionEnum::FOD_Check_Detection); },
               [](MachineObject* o) { return detection_value(o, PrintOptionEnum::FOD_Check_Detection); },
               [](MachineObject* o, int v) { opts_of(o)->command_xcam_control_fod_check(v != 0); });

    add_toggle(_L("Displacement detection"), _L("Pause if the model is detected as having shifted on the plate."),
               [](MachineObject* o) { return detection_supported(o, PrintOptionEnum::Displacement_Detection); },
               [](MachineObject* o) { return detection_value(o, PrintOptionEnum::Displacement_Detection); },
               [](MachineObject* o, int v) { opts_of(o)->command_xcam_control_displacement_detection(v != 0); });

    add_choice(_L("Nozzle clumping detection"),
               _L("Detects filament or debris wrapped around the nozzle. Auto disables it for leak-prone filaments."),
               {_L("Off"), _L("On"), _L("Auto")},
               [](MachineObject* o) { return detection_supported(o, PrintOptionEnum::Smart_Nozzle_Blob_Detection); },
               [](MachineObject* o) { return detection_value(o, PrintOptionEnum::Smart_Nozzle_Blob_Detection); },
               [](MachineObject* o, int v) { opts_of(o)->command_smart_nozzle_blob_detect_mode(v); });

    /* ---- After the print ----------------------------------------------------------- */
    add_section(_L("After the print"));

    add_choice(_L("Purify chamber air"), _L("Runs the filtration when a job finishes."),
               {_L("Off"), _L("Internal circulation"), _L("Exhaust outside")},
               [](MachineObject* o) { return detection_supported(o, PrintOptionEnum::Purify_Air_At_Print_End); },
               [](MachineObject* o) { return detection_value(o, PrintOptionEnum::Purify_Air_At_Print_End); },
               [](MachineObject* o, int v) { opts_of(o)->command_xcam_control_purify_air_at_print_end(v); });

    add_toggle(_L("Print status snapshot"),
               _L("Photograph finished parts and upload them to the Bambu cloud."),
               [](MachineObject* o) { return detection_supported(o, PrintOptionEnum::Snapshot_Detection); },
               [](MachineObject* o) { return detection_value(o, PrintOptionEnum::Snapshot_Detection); },
               [](MachineObject* o, int v) { opts_of(o)->command_snapshot_control(v); });

    /* ---- Camera -------------------------------------------------------------------- */
    add_section(_L("Camera"));

    add_toggle(_L("Record while printing"), _L("Save a recording of each job to the printer's storage."),
               [](MachineObject* o) { return true; },
               [](MachineObject* o) { return o->camera_recording_when_printing ? 1 : 0; },
               [](MachineObject* o, int v) { o->command_ipcam_record(v != 0); },
               /* disable_while_printing */ true);

    add_toggle(_L("Timelapse"), _L("Capture a timelapse of each job."),
               [](MachineObject* o) { return o->is_support_timelapse; },
               [](MachineObject* o) { return o->camera_timelapse ? 1 : 0; },
               [](MachineObject* o, int v) { o->command_ipcam_timelapse(v != 0); },
               /* disable_while_printing */ true);

    add_choice(_L("Resolution"), wxEmptyString, {"720p", "1080p"},
               [](MachineObject* o) { return o->camera_resolution_supported.size() > 1; },
               [](MachineObject* o) { return o->camera_resolution == "1080p" ? 1 : 0; },
               [](MachineObject* o, int v) { o->command_ipcam_resolution_set(v == 1 ? "1080p" : "720p"); },
               /* disable_while_printing */ true);

    /* ---- AMS ----------------------------------------------------------------------- */
    add_section(_L("AMS"));

    add_toggle(_L("Read filament on power-up"), wxEmptyString,
               [](MachineObject* o) { return o->HasAms() && ams_setting_of(o) != nullptr; },
               [](MachineObject* o) { return ams_setting_of(o)->IsDetectOnPowerupEnabled() ? 1 : 0; },
               [](MachineObject* o, int v) {
                   DevAmsSystemSetting* s = ams_setting_of(o);
                   /* The firmware takes all three together, so the two we are not changing
                      have to be sent back at their current values. */
                   o->command_ams_user_settings(v != 0, s->IsDetectRemainEnabled(), s->IsDetectRemainEnabled());
               });

    add_toggle(_L("Estimate filament remaining"), wxEmptyString,
               [](MachineObject* o) { return o->HasAms() && o->is_support_update_remain && ams_setting_of(o) != nullptr; },
               [](MachineObject* o) { return ams_setting_of(o)->IsDetectRemainEnabled() ? 1 : 0; },
               [](MachineObject* o, int v) {
                   o->command_ams_user_settings(ams_setting_of(o)->IsDetectOnPowerupEnabled(), v != 0, v != 0);
               });

    add_toggle(_L("Auto-refill from a matching spool"),
               _L("Switch to another slot holding the same filament when one runs out."),
               [](MachineObject* o) { return o->HasAms() && o->is_support_filament_backup && ams_setting_of(o) != nullptr; },
               [](MachineObject* o) { return ams_setting_of(o)->IsAutoRefillEnabled() ? 1 : 0; },
               [](MachineObject* o, int v) { o->command_ams_switch_filament(v != 0); });

    add_toggle(_L("Air print detection"), _L("Detect printing with no filament actually coming out."),
               [](MachineObject* o) { return o->HasAms() && o->is_support_air_print_detection; },
               [](MachineObject* o) { return o->ams_air_print_status ? 1 : 0; },
               [](MachineObject* o, int v) { o->command_ams_air_print_detect(v != 0); });

    SetSizer(m_sizer);
    Layout();
    refresh_rows();
}

void DeviceSettingsPanel::refresh_rows()
{
    /* Guard the whole refresh: SetValue/SetSelection can emit a change event on some
       platforms, and that would send the value we just read straight back to the printer. */
    m_updating = true;

    const bool connected = m_obj != nullptr;
    const bool printing  = connected && m_obj->is_in_printing();

    int shown = 0;
    for (auto& section : m_sections) {
        int section_shown = 0;
        for (Row* row : section.second) {
            bool ok = false;
            try {
                ok = connected && row->supported && row->supported(m_obj);
            } catch (const std::exception&) {
                ok = false;
            }

            row->panel->Show(ok);
            if (!ok)
                continue;
            ++section_shown;

            const int value = row->read ? row->read(m_obj) : -1;
            if (row->check != nullptr) {
                if (value >= 0)
                    row->check->SetValue(value != 0);
            } else if (row->choice != nullptr) {
                if (value >= 0 && value < (int) row->choice->GetCount())
                    row->choice->SetSelection(value);
            }

            const bool enabled = !(printing && row->disable_while_printing);
            if (row->check != nullptr)
                row->check->Enable(enabled);
            if (row->choice != nullptr)
                row->choice->Enable(enabled);
            if (row->caption != nullptr)
                row->caption->Enable(enabled);
        }
        section.first->Show(section_shown > 0);
        shown += section_shown;
    }

    m_empty->Show(!connected);
    if (!connected)
        m_empty->SetLabel(_L("Connect to a printer to see the settings it supports."));
    else if (shown == 0)
        m_empty->SetLabel(_L("This printer has not reported any settings that can be changed from here."));
    m_empty->Show(!connected || shown == 0);

    if (printing) {
        m_banner->SetLabel(_L("A job is running. Settings that would disturb it - the camera options - "
                              "are disabled until it finishes. The rest take effect straight away."));
        m_banner->Wrap(std::max(FromDIP(300), GetClientSize().GetWidth() - FromDIP(40)));
        m_banner->Show();
    } else {
        m_banner->Hide();
    }

    m_updating = false;

    Layout();
    FitInside();
}

void DeviceSettingsPanel::update(MachineObject* obj)
{
    m_obj = obj;
    if (IsShownOnScreen() || obj == nullptr)
        refresh_rows();
}

void DeviceSettingsPanel::show_status(int status)
{
    if (m_last_status == status)
        return;
    m_last_status = status;

    if ((status & (int) MonitorStatus::MONITOR_NORMAL) == 0)
        m_obj = nullptr;
    refresh_rows();
}

}} // namespace Slic3r::GUI

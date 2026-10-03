#include "BedPositionCtrl.hpp"

#include <algorithm>
#include <cmath>

#include <wx/dcbuffer.h>
#include <wx/dcgraph.h>

#include "Label.hpp"
#include "StateColor.hpp"

#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"

namespace Slic3r { namespace GUI {

static const double GRID_MM = 50.0;

/* A drawn stroke is sampled rather than followed pixel by pixel: below this spacing the
   extra points add nothing you can see and plenty the planner has to chew through. */
static const double MIN_PATH_STEP_MM = 3.0;
static const int    MAX_PATH_POINTS  = 240;

BedPositionCtrl::BedPositionCtrl(wxWindow* parent) : wxPanel(parent, wxID_ANY)
{
    SetBackgroundStyle(wxBG_STYLE_PAINT); /* required for wxAutoBufferedPaintDC */
    SetBackgroundColour(parent->GetBackgroundColour());

    Bind(wxEVT_PAINT, &BedPositionCtrl::OnPaint, this);
    Bind(wxEVT_LEFT_DOWN, &BedPositionCtrl::OnDown, this);
    Bind(wxEVT_LEFT_UP, &BedPositionCtrl::OnClick, this);
    Bind(wxEVT_MOTION, &BedPositionCtrl::OnMotion, this);
    Bind(wxEVT_LEAVE_WINDOW, &BedPositionCtrl::OnLeave, this);
    Bind(wxEVT_MOUSE_CAPTURE_LOST, &BedPositionCtrl::OnCaptureLost, this);
}

void BedPositionCtrl::SetBedSize(double width_mm, double depth_mm)
{
    if (width_mm <= 0 || depth_mm <= 0)
        return;
    if (width_mm == m_bed_w && depth_mm == m_bed_d)
        return;
    m_bed_w = width_mm;
    m_bed_d = depth_mm;
    Refresh();
}

void BedPositionCtrl::SetPosition(const std::optional<wxRealPoint>& pos_mm)
{
    if (m_pos.has_value() == pos_mm.has_value() &&
        (!m_pos.has_value() || (m_pos->x == pos_mm->x && m_pos->y == pos_mm->y)))
        return;
    m_pos = pos_mm;
    Refresh();
}

void BedPositionCtrl::SetUnavailableReason(const wxString& reason)
{
    if (reason == m_unavailable)
        return;
    m_unavailable = reason;
    Refresh();
}

/* Largest rectangle with the bed's aspect ratio that fits, centred, leaving room for the
   axis labels along the bottom and left. */
wxRect BedPositionCtrl::PlateRect() const
{
    const wxSize size = GetClientSize();
    const int    pad  = FromDIP(18);

    const int avail_w = size.x - pad * 2;
    const int avail_h = size.y - pad * 2;
    if (avail_w <= 0 || avail_h <= 0 || m_bed_w <= 0 || m_bed_d <= 0)
        return wxRect();

    const double aspect = m_bed_w / m_bed_d;
    int w = avail_w;
    int h = (int) std::lround(w / aspect);
    if (h > avail_h) {
        h = avail_h;
        w = (int) std::lround(h * aspect);
    }
    return wxRect(pad + (avail_w - w) / 2, pad + (avail_h - h) / 2, w, h);
}

/* Y is flipped: the plate's origin is front-left, the screen's is top-left. */
bool BedPositionCtrl::ToMm(const wxPoint& px, double& x_mm, double& y_mm) const
{
    const wxRect r = PlateRect();
    if (r.width <= 0 || r.height <= 0)
        return false;
    if (!r.Contains(px))
        return false;

    x_mm = (double) (px.x - r.x) / r.width * m_bed_w;
    y_mm = (double) (r.y + r.height - px.y) / r.height * m_bed_d;
    return true;
}

wxPoint BedPositionCtrl::ToPixels(double x_mm, double y_mm) const
{
    const wxRect r = PlateRect();
    return wxPoint(r.x + (int) std::lround(x_mm / m_bed_w * r.width),
                   r.y + r.height - (int) std::lround(y_mm / m_bed_d * r.height));
}

void BedPositionCtrl::OnPaint(wxPaintEvent& event)
{
    wxAutoBufferedPaintDC dc(this);
    dc.SetBackground(wxBrush(GetBackgroundColour()));
    dc.Clear();

    const bool   dark  = wxGetApp().dark_mode();
    const wxRect plate = PlateRect();

    const wxColour plate_bg   = dark ? wxColour(0x2A, 0x2E, 0x30) : wxColour(0xF0, 0xF0, 0xF0);
    const wxColour grid_col   = dark ? wxColour(0x44, 0x4A, 0x4D) : wxColour(0xD2, 0xD2, 0xD2);
    const wxColour border_col = dark ? wxColour(0x66, 0x6C, 0x70) : wxColour(0xA0, 0xA0, 0xA0);
    const wxColour text_col   = dark ? wxColour(0x9A, 0x9A, 0x9A) : wxColour(0x6B, 0x6B, 0x6B);
    const wxColour head_col   = wxColour(0x00, 0xAE, 0x42);
    const wxColour hover_col  = dark ? wxColour(0x6A, 0xB8, 0xF0) : wxColour(0x1B, 0x6B, 0xA8);

    if (plate.width <= 0) {
        dc.SetTextForeground(text_col);
        dc.SetFont(Label::Body_12);
        dc.DrawLabel(_L("Bed size unknown"), GetClientRect(), wxALIGN_CENTER);
        return;
    }

    /* plate */
    dc.SetBrush(wxBrush(plate_bg));
    dc.SetPen(wxPen(border_col));
    dc.DrawRectangle(plate);

    /* 50mm grid, drawn from the origin outwards so the lines land on round numbers */
    dc.SetPen(wxPen(grid_col, 1, wxPENSTYLE_DOT));
    for (double x = GRID_MM; x < m_bed_w; x += GRID_MM) {
        const int px = plate.x + (int) std::lround(x / m_bed_w * plate.width);
        dc.DrawLine(px, plate.y + 1, px, plate.y + plate.height - 1);
    }
    for (double y = GRID_MM; y < m_bed_d; y += GRID_MM) {
        const int py = plate.y + plate.height - (int) std::lround(y / m_bed_d * plate.height);
        dc.DrawLine(plate.x + 1, py, plate.x + plate.width - 1, py);
    }

    /* axis extents, so the grid has a scale */
    dc.SetTextForeground(text_col);
    dc.SetFont(Label::Body_10);
    dc.DrawText("0", plate.x, plate.y + plate.height + FromDIP(2));
    const wxString x_max = wxString::Format("%d", (int) std::lround(m_bed_w));
    dc.DrawText(x_max, plate.x + plate.width - dc.GetTextExtent(x_max).x, plate.y + plate.height + FromDIP(2));
    const wxString y_max = wxString::Format("%d", (int) std::lround(m_bed_d));
    dc.DrawText(y_max, plate.x - dc.GetTextExtent(y_max).x - FromDIP(4), plate.y);

    if (!m_unavailable.empty()) {
        /* Dim the plate and say why, rather than leaving a live-looking grid that ignores
           clicks. */
        dc.SetBrush(wxBrush(wxColour(plate_bg.Red(), plate_bg.Green(), plate_bg.Blue(), 200)));
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.DrawRectangle(plate);
        dc.SetTextForeground(text_col);
        dc.SetFont(Label::Body_12);
        dc.DrawLabel(m_unavailable, plate, wxALIGN_CENTER);
        return;
    }

    /* hover crosshair and read-out */
    if (m_hover.has_value()) {
        const wxPoint h = ToPixels(m_hover->x, m_hover->y);
        dc.SetPen(wxPen(hover_col, 1, wxPENSTYLE_SHORT_DASH));
        dc.DrawLine(plate.x, h.y, plate.x + plate.width, h.y);
        dc.DrawLine(h.x, plate.y, h.x, plate.y + plate.height);

        dc.SetTextForeground(hover_col);
        dc.SetFont(Label::Body_10);
        const wxString label = wxString::Format("X%.0f Y%.0f", m_hover->x, m_hover->y);
        dc.DrawText(label, plate.x + FromDIP(4), plate.y + FromDIP(3));
    }

    /* the stroke being drawn */
    if (m_path.size() >= 2) {
        dc.SetPen(wxPen(hover_col, FromDIP(2)));
        for (size_t i = 1; i < m_path.size(); ++i)
            dc.DrawLine(ToPixels(m_path[i - 1].x, m_path[i - 1].y), ToPixels(m_path[i].x, m_path[i].y));

        dc.SetTextForeground(hover_col);
        dc.SetFont(Label::Body_10);
        dc.DrawText(wxString::Format(_L("%d points"), (int) m_path.size()),
                    plate.x + FromDIP(4), plate.y + plate.height - FromDIP(16));
    }

    /* the head */
    if (m_pos.has_value()) {
        const wxPoint p = ToPixels(m_pos->x, m_pos->y);
        const int     r = FromDIP(4);
        dc.SetBrush(wxBrush(head_col));
        dc.SetPen(wxPen(head_col));
        dc.DrawCircle(p, r);
        dc.SetBrush(*wxTRANSPARENT_BRUSH);
        dc.DrawCircle(p, r + FromDIP(3));
    }
}

void BedPositionCtrl::OnDown(wxMouseEvent& event)
{
    event.Skip();
    if (!m_unavailable.empty())
        return;

    double x = 0, y = 0;
    if (!ToMm(event.GetPosition(), x, y))
        return;

    m_drawing = true;
    m_path.clear();
    m_path.push_back(wxRealPoint(x, y));
    if (!HasCapture())
        CaptureMouse();
    Refresh();
}

void BedPositionCtrl::OnMotion(wxMouseEvent& event)
{
    event.Skip();
    if (!m_unavailable.empty()) {
        if (m_hover.has_value()) {
            m_hover.reset();
            Refresh();
        }
        return;
    }

    double x = 0, y = 0;
    const bool inside = ToMm(event.GetPosition(), x, y);

    if (m_drawing && event.LeftIsDown()) {
        /* Only record a point once the pointer has travelled far enough to be worth its own
           move. Without this a single stroke becomes hundreds of G1s and the payload (and
           the printer's planner) suffers for no visible gain. */
        if (inside && !m_path.empty()) {
            const wxRealPoint& last = m_path.back();
            if (std::hypot(x - last.x, y - last.y) >= MIN_PATH_STEP_MM && (int) m_path.size() < MAX_PATH_POINTS)
                m_path.push_back(wxRealPoint(x, y));
        }
    }

    if (inside) {
        m_hover = wxRealPoint(x, y);
        SetCursor(wxCursor(wxCURSOR_CROSS));
    } else {
        m_hover.reset();
        SetCursor(wxNullCursor);
    }
    Refresh();
}

void BedPositionCtrl::OnLeave(wxMouseEvent& event)
{
    event.Skip();
    if (m_hover.has_value()) {
        m_hover.reset();
        Refresh();
    }
}

void BedPositionCtrl::OnCaptureLost(wxMouseCaptureLostEvent& event)
{
    /* Dropped capture means the gesture is void - send nothing. */
    m_drawing = false;
    m_path.clear();
    Refresh();
}

void BedPositionCtrl::OnClick(wxMouseEvent& event)
{
    event.Skip();
    if (HasCapture())
        ReleaseMouse();

    const bool was_drawing = m_drawing;
    m_drawing              = false;

    if (!m_unavailable.empty()) {
        m_path.clear();
        Refresh();
        return;
    }

    /* ToMm rejects anything outside the plate, so an out-of-bounds release sends nothing. */
    double x = 0, y = 0;
    const bool inside = ToMm(event.GetPosition(), x, y);

    /* A stroke of two or more points is a path; anything shorter was a click. */
    if (was_drawing && m_path.size() >= 2) {
        if (m_on_path)
            m_on_path(m_path);
        m_pos = m_path.back();
        m_path.clear();
        Refresh();
        return;
    }

    m_path.clear();
    Refresh();

    if (!inside || !m_on_move)
        return;
    m_on_move(std::clamp(x, 0.0, m_bed_w), std::clamp(y, 0.0, m_bed_d));
}

}} // namespace Slic3r::GUI

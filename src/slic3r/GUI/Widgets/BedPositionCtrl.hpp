#ifndef slic3r_GUI_BedPositionCtrl_hpp_
#define slic3r_GUI_BedPositionCtrl_hpp_

#include <functional>
#include <optional>
#include <vector>

#include <wx/panel.h>

namespace Slic3r { namespace GUI {

/* Top-down view of the build plate: a 50mm grid, the toolhead's position, and click to move.
 *
 * On the position marker: the printer never reports where its head is. Nothing in the status
 * push carries X/Y/Z - it reports temperatures, progress, state and errors, and that is all.
 * So the marker can only show where we last *told* the head to go, which is accurate exactly
 * while the printer is idle and we are the only one issuing moves. The moment a job starts,
 * the firmware moves the head constantly without telling us, so the marker is hidden rather
 * than left behind to lie about it.
 *
 * The control disables itself unless the printer is idle and both X and Y are homed, because
 * an absolute move is only meaningful once the machine agrees where zero is.
 */
class BedPositionCtrl : public wxPanel
{
public:
    BedPositionCtrl(wxWindow* parent);

    /* Printable area in mm. Anything outside it cannot be clicked. */
    void SetBedSize(double width_mm, double depth_mm);

    /* nullopt hides the marker. */
    void SetPosition(const std::optional<wxRealPoint>& pos_mm);
    std::optional<wxRealPoint> GetPosition() const { return m_pos; }

    /* Why the control is unavailable, shown in place of the grid. Empty means usable. */
    void SetUnavailableReason(const wxString& reason);

    /* Called with a clamped, in-bounds position in mm when the user clicks the plate. */
    void SetOnMove(std::function<void(double, double)> fn) { m_on_move = std::move(fn); }

    /* Called with a drawn path once the mouse is released. Always two points or more. */
    void SetOnPath(std::function<void(const std::vector<wxRealPoint>&)> fn) { m_on_path = std::move(fn); }

private:
    void OnPaint(wxPaintEvent& event);
    void OnDown(wxMouseEvent& event);
    void OnClick(wxMouseEvent& event);
    void OnMotion(wxMouseEvent& event);
    void OnLeave(wxMouseEvent& event);
    void OnCaptureLost(wxMouseCaptureLostEvent& event);

    /* The plate rectangle in device pixels, kept square to the bed's aspect ratio. */
    wxRect PlateRect() const;
    bool   ToMm(const wxPoint& px, double& x_mm, double& y_mm) const;
    wxPoint ToPixels(double x_mm, double y_mm) const;

    double m_bed_w = 0.0;
    double m_bed_d = 0.0;

    std::optional<wxRealPoint> m_pos;
    std::optional<wxRealPoint> m_hover;
    wxString                   m_unavailable;

    /* Path being drawn, in mm. Points are only added once the pointer has moved far enough
       to be worth a move command, so a shaky hand does not become a thousand G1s. */
    std::vector<wxRealPoint> m_path;
    bool                     m_drawing = false;

    std::function<void(double, double)>                     m_on_move;
    std::function<void(const std::vector<wxRealPoint>&)>    m_on_path;
};

}} // namespace Slic3r::GUI

#endif // slic3r_GUI_BedPositionCtrl_hpp_

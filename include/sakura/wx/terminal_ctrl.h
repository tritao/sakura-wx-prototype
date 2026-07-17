#pragma once

#include <sakura/terminal/core.h>
#include <sakura/terminal/transport.h>

#include <wx/wx.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

class WxTerminalCtrl final : public wxPanel {
public:
    explicit WxTerminalCtrl(wxWindow* parent,
                            std::unique_ptr<TerminalTransport> transport = nullptr);
    ~WxTerminalCtrl() override;

    TerminalCore& Core() { return core_; }
    const TerminalCore& Core() const { return core_; }

    // Deterministic interaction hook used by the UX smoke test and embedders.
    bool RunScenario();

private:
    void UpdateGeometry();
    static uint32_t KeySymFor(const wxKeyEvent& event);
    void OnPaint(wxPaintEvent& event);
    void OnSize(wxSizeEvent& event);
    void OnChar(wxKeyEvent& event);
    void OnMouseWheel(wxMouseEvent& event);
    std::pair<unsigned int, unsigned int> CellAt(const wxPoint& point) const;
    static unsigned char MouseModifiers(const wxMouseEvent& event);
    bool ForwardMouse(const wxMouseEvent& event, unsigned int button,
                      unsigned int mouse_event);
    void BeginMouseReporting(unsigned int button);
    void EndMouseReporting(const wxMouseEvent& event);
    void OnLeftDown(wxMouseEvent& event);
    void OnLeftUp(wxMouseEvent& event);
    void OnMouseButtonDown(wxMouseEvent& event);
    void OnMouseButtonUp(wxMouseEvent& event);
    void UpdateSelectionAt(const wxPoint& position);
    void OnMotion(wxMouseEvent& event);
    bool CopySelectionToClipboard();
    bool PasteFromClipboard();
    wxString TransportTitle(const TransportStatus& status) const;
    void UpdateTransportTitle(const TransportStatus& status);
    void ShowTransportNotice(const TransportStatus& status);
    void RestartTransport();
    void UpdateTransportStatus();
    void OnTimer(wxTimerEvent& event);

    wxFont font_;
    wxTimer output_timer_;
    std::unique_ptr<TerminalTransport> transport_;
    TerminalCore core_;
    wxString error_;
    int cell_width_ = 8;
    int cell_height_ = 16;
    unsigned int columns_ = 80;
    unsigned int rows_ = 24;
    bool selection_dragging_ = false;
    bool pointer_down_ = false;
    bool mouse_reporting_gesture_ = false;
    bool trace_metrics_ = false;
    int click_count_ = 0;
    wxPoint pointer_down_position_;
    unsigned int selection_anchor_column_ = 0;
    unsigned int selection_anchor_row_ = 0;
    unsigned int last_click_column_ = 0;
    unsigned int last_click_row_ = 0;
    unsigned int mouse_reporting_button_ = TSM_MOUSE_BUTTON_LEFT;
    int auto_scroll_direction_ = 0;
    wxPoint last_pointer_position_;
    std::chrono::steady_clock::time_point last_click_time_;
    TransportState last_transport_state_ = TransportState::Stopped;
    std::chrono::steady_clock::time_point last_metrics_log_;
};

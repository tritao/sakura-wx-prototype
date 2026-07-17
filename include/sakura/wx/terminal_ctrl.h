#pragma once

#include <sakura/terminal/core.h>
#include <sakura/terminal/transport.h>

#include <wx/wx.h>

#include <cstdint>
#include <memory>
#include <utility>

class WxTerminalCtrl final : public wxPanel {
public:
    explicit WxTerminalCtrl(wxWindow* parent,
                            std::unique_ptr<TerminalTransport> transport = nullptr);
    ~WxTerminalCtrl() override;

    TerminalCore& Core();
    const TerminalCore& Core() const;

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

    class Impl;
    std::unique_ptr<Impl> impl_;
};

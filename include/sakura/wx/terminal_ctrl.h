#pragma once

#include <sakura/wx/config.h>

#include <wx/wx.h>

#include <cstdint>
#include <memory>
#include <utility>

class WxTerminalCtrl final : public wxPanel {
public:
    // The control owns the supplied transport and stops it before destruction.
    // All control methods and callbacks are UI-thread-affine.
    explicit WxTerminalCtrl(wxWindow* parent,
                            std::unique_ptr<TerminalTransport> transport = nullptr,
                            TerminalConfig config = {},
                            TerminalCallbacks callbacks = {});
    ~WxTerminalCtrl() override;

    TerminalCore& Core();
    const TerminalCore& Core() const;

private:
    void UpdateGeometry();
    static uint32_t KeySymFor(const wxKeyEvent& event);
    const wxFont& GlyphFont(uint8_t attributes);
    const wxString& GlyphText(const std::string& text);
    void RenderSnapshot(wxDC& dc, const TerminalSnapshot& snapshot,
                        const TerminalDirtyRegion& dirty);
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
    void NotifyTitleChanged();
    void NotifyTransportStatus(const TransportStatus& status);
    void ReportError(const std::string& message);
    void ShowTransportNotice(const TransportStatus& status);
    void RestartTransport();
    void UpdateTransportStatus();
    void OnTimer(wxTimerEvent& event);

    class Impl;
    std::unique_ptr<Impl> impl_;
};

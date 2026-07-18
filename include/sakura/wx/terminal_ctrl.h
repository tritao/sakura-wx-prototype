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

    SakuraTerminal* Core();
    const SakuraTerminal* Core() const;
    // Returns the resolved wx face name, or a descriptive fallback when the
    // platform keeps the generic monospace family unresolved.
    std::string GetFontFamily() const;
    int GetFontSize() const;
    wxSize GetCellSize() const;
    // Capture the latest core frame and schedule the smallest appropriate wx
    // invalidation. Hosts that mutate Core() directly should call this after
    // feeding output or changing terminal state.
    void RefreshFrame();
    WxPaintMetrics GetPaintMetrics() const;

private:
    void RequestFrameRefresh();
    void UpdateGeometry();
    static uint32_t KeySymFor(const wxKeyEvent& event);
    const wxFont& GlyphFont(uint8_t attributes);
    const wxString& GlyphText(const char* text, std::size_t length);
    const wxBitmap& GlyphRunBitmap(
        wxDC& dc, const SakuraTerminalRunView& run, const wxString& text,
        const std::array<uint8_t, 3>& foreground,
        const std::array<uint8_t, 3>& background);
    void RenderFrame(wxDC& dc, const SakuraTerminalFrame* frame,
                     const SakuraTerminalFrameInfo& info,
                     const SakuraTerminalDirtyRegion& dirty,
                        uint64_t* painted_cells);
    void OnPaint(wxPaintEvent& event);
    void OnSize(wxSizeEvent& event);
    void OnChar(wxKeyEvent& event);
    void OnMouseWheel(wxMouseEvent& event);
    void QueueWheelScroll(const wxMouseEvent& event);
    bool FlushWheelScroll();
    void BeginScrollAnimation(const SakuraTerminalFrameInfo& info);
    void CancelScrollAnimation(bool forced);
    bool AdvanceScrollAnimation();
    void DrawScrollAnimation(wxDC& dc);
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

#pragma once

#include <sakura/wx/config.h>

#include <wx/bitmap.h>
#include <wx/dc.h>
#include <wx/font.h>
#include <wx/gdicmn.h>
#include <wx/string.h>
#include <wx/window.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

class WxRenderer final {
public:
    WxRenderer(wxWindow& owner, const TerminalConfig& config,
               WxPaintMetrics& metrics);
    ~WxRenderer();

    WxRenderer(const WxRenderer&) = delete;
    WxRenderer& operator=(const WxRenderer&) = delete;

    bool UpdateGeometry();
    const wxFont& Font() const;
    std::string FontFamily() const;
    int FontSize() const;
    wxSize CellSize() const;
    int CellWidth() const;
    int CellHeight() const;
    unsigned int Columns() const;
    unsigned int Rows() const;

    void RecordPaintDuration(uint64_t elapsed_us);
    void PaintFrame(wxDC& dc, const SakuraTerminalFrame* frame,
                    const SakuraTerminalFrameInfo& info,
                    const SakuraTerminalDirtyRegion& dirty,
                    bool full_repaint, uint64_t* painted_cells);
    void Draw(wxDC& dc);

    bool AdvanceScrollAnimation();
    void CancelScrollAnimation(bool forced, const char* reason = nullptr);
    bool ScrollAnimationActive() const;
    void SetTraceScroll(bool enabled);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

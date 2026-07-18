#include <sakura/wx/terminal_ctrl.h>

#include <wx/dcclient.h>
#include <wx/dcmemory.h>
#include <wx/frame.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iostream>
#include <string>

namespace {

void SendWheel(WxTerminalCtrl& terminal, int rotation, int delta = 120,
               int lines_per_action = 3)
{
    wxMouseEvent event(wxEVT_MOUSEWHEEL);
    event.SetEventObject(&terminal);
    event.m_wheelAxis = wxMOUSE_WHEEL_VERTICAL;
    event.m_wheelRotation = rotation;
    event.m_wheelDelta = delta;
    event.m_wheelInverted = false;
    event.m_linesPerAction = lines_per_action;
    event.m_columnsPerAction = 3;
    terminal.GetEventHandler()->ProcessEvent(event);
}

void Tick(WxTerminalCtrl& terminal)
{
    wxTimer timer(&terminal);
    wxTimerEvent event(timer);
    terminal.GetEventHandler()->ProcessEvent(event);
}

void TickAndPaint(WxTerminalCtrl& terminal)
{
    Tick(terminal);
    wxYield();
    terminal.Update();
}

wxBitmap CaptureClient(WxTerminalCtrl& terminal)
{
    const wxSize size = terminal.GetClientSize();
    wxBitmap bitmap(std::max(1, size.GetWidth()),
                    std::max(1, size.GetHeight()), -1);
    wxMemoryDC target(bitmap);
    wxClientDC source(&terminal);
    target.Blit(0, 0, bitmap.GetWidth(), bitmap.GetHeight(), &source, 0, 0,
                wxCOPY);
    target.SelectObject(wxNullBitmap);
    return bitmap;
}

bool IsBackground(const unsigned char* pixel)
{
    constexpr unsigned char background[] = {16, 18, 20};
    return std::abs(static_cast<int>(pixel[0]) - background[0]) <= 3 &&
           std::abs(static_cast<int>(pixel[1]) - background[1]) <= 3 &&
           std::abs(static_cast<int>(pixel[2]) - background[2]) <= 3;
}

bool SamePixel(const unsigned char* left, const unsigned char* right)
{
    return std::abs(static_cast<int>(left[0]) - right[0]) <= 5 &&
           std::abs(static_cast<int>(left[1]) - right[1]) <= 5 &&
           std::abs(static_cast<int>(left[2]) - right[2]) <= 5;
}

std::size_t DifferentPixels(const wxBitmap& left, const wxBitmap& right)
{
    const wxImage left_image = left.ConvertToImage();
    const wxImage right_image = right.ConvertToImage();
    if (!left_image.IsOk() || !right_image.IsOk() ||
        left_image.GetWidth() != right_image.GetWidth() ||
        left_image.GetHeight() != right_image.GetHeight())
        return static_cast<std::size_t>(-1);

    const unsigned char* left_data = left_image.GetData();
    const unsigned char* right_data = right_image.GetData();
    const std::size_t pixels = static_cast<std::size_t>(left_image.GetWidth()) *
        static_cast<std::size_t>(left_image.GetHeight());
    std::size_t different = 0;
    for (std::size_t index = 0; index < pixels; ++index) {
        if (!SamePixel(left_data + index * 3, right_data + index * 3))
            ++different;
    }
    return different;
}

struct ShiftMatch {
    int shift = 0;
    std::size_t compared = 0;
    std::size_t matched = 0;
};

ShiftMatch BestVerticalShift(const wxBitmap& before, const wxBitmap& after,
                             int max_shift)
{
    const wxImage before_image = before.ConvertToImage();
    const wxImage after_image = after.ConvertToImage();
    ShiftMatch best;
    if (!before_image.IsOk() || !after_image.IsOk() ||
        before_image.GetWidth() != after_image.GetWidth() ||
        before_image.GetHeight() != after_image.GetHeight())
        return best;

    const int width = before_image.GetWidth();
    const int height = before_image.GetHeight();
    const unsigned char* before_data = before_image.GetData();
    const unsigned char* after_data = after_image.GetData();
    max_shift = std::min(max_shift, height - 1);
    for (int shift = -max_shift; shift <= max_shift; ++shift) {
        if (shift == 0)
            continue;
        const int before_top = std::max(0, -shift);
        const int before_bottom = std::min(height, height - shift);
        ShiftMatch candidate;
        candidate.shift = shift;
        for (int y = before_top; y < before_bottom; ++y) {
            const int after_y = y + shift;
            for (int x = 0; x < width; ++x) {
                const unsigned char* before_pixel = before_data +
                    (static_cast<std::size_t>(y) * width + x) * 3;
                const unsigned char* after_pixel = after_data +
                    (static_cast<std::size_t>(after_y) * width + x) * 3;
                if (IsBackground(before_pixel) && IsBackground(after_pixel))
                    continue;
                ++candidate.compared;
                if (SamePixel(before_pixel, after_pixel))
                    ++candidate.matched;
            }
        }
        if (candidate.compared < 100 ||
            (best.compared != 0 &&
             candidate.matched * best.compared <=
                 best.matched * candidate.compared))
            continue;
        if (candidate.matched > best.matched || best.compared == 0)
            best = candidate;
    }
    return best;
}

int RunScrollTest()
{
    wxFrame frame(nullptr, wxID_ANY, "Sakura scroll test", wxDefaultPosition,
                 wxSize(640, 480));
    TerminalConfig config;
    config.start_transport = false;
    config.timer_interval_ms = 60000;
    config.smooth_scrolling = false;
    WxTerminalCtrl terminal(&frame, nullptr, config);

    const WxPaintMetrics initial = terminal.GetPaintMetrics();
    SendWheel(terminal, 60);
    const WxPaintMetrics partial = terminal.GetPaintMetrics();
    if (partial.wheel_events != initial.wheel_events + 1 ||
        partial.wheel_partial_events != initial.wheel_partial_events + 1 ||
        partial.wheel_scroll_updates != initial.wheel_scroll_updates) {
        std::cerr << "partial wheel rotation was applied too early\n";
        return 1;
    }

    Tick(terminal);
    const WxPaintMetrics still_partial = terminal.GetPaintMetrics();
    if (still_partial.wheel_scroll_updates != initial.wheel_scroll_updates) {
        std::cerr << "partial wheel rotation changed viewport\n";
        return 2;
    }

    SendWheel(terminal, 60);
    Tick(terminal);
    const WxPaintMetrics first_line = terminal.GetPaintMetrics();
    if (first_line.wheel_scroll_updates != initial.wheel_scroll_updates + 1 ||
        first_line.wheel_lines_scrolled != initial.wheel_lines_scrolled + 1) {
        std::cerr << "one accumulated wheel action did not scroll one line\n";
        return 3;
    }

    Tick(terminal);
    Tick(terminal);
    const WxPaintMetrics completed_action = terminal.GetPaintMetrics();
    if (completed_action.wheel_scroll_updates != initial.wheel_scroll_updates + 3 ||
        completed_action.wheel_lines_scrolled != initial.wheel_lines_scrolled + 3) {
        std::cerr << "wheel action was not drained at one line per tick\n";
        return 4;
    }

    SendWheel(terminal, 120);
    SendWheel(terminal, 120);
    const WxPaintMetrics queued = terminal.GetPaintMetrics();
    if (queued.wheel_scroll_updates != completed_action.wheel_scroll_updates) {
        std::cerr << "wheel events were applied before the UI tick\n";
        return 5;
    }
    Tick(terminal);
    const WxPaintMetrics coalesced = terminal.GetPaintMetrics();
    if (coalesced.wheel_scroll_updates != queued.wheel_scroll_updates + 1 ||
        coalesced.wheel_lines_scrolled != queued.wheel_lines_scrolled + 1) {
        std::cerr << "wheel burst was not coalesced and smoothed\n";
        return 6;
    }

    return 0;
}

int RunAnimationTest()
{
    wxFrame frame(nullptr, wxID_ANY, "Sakura scroll animation test",
                 wxDefaultPosition, wxSize(640, 480));
    TerminalConfig config;
    config.start_transport = false;
    config.timer_interval_ms = 60000;
    config.scroll_animation_ms_per_line = 50;
    config.scroll_animation_max_ms = 100;
    WxTerminalCtrl terminal(&frame, nullptr, config);
    terminal.SetSize(wxSize(640, 480));
    frame.Show();
    wxYield();

    std::string output;
    for (unsigned int line = 0; line < 64; ++line)
        output += "animation-line-" + std::to_string(line) + "\r\n";
    sakura_terminal_feed_output(terminal.Core(), output.data(), output.size());
    terminal.RefreshFrame();
    wxYield();
    terminal.Update();
    const WxPaintMetrics initial = terminal.GetPaintMetrics();
    const wxBitmap before = CaptureClient(terminal);

    constexpr int expected_lines = 2;
    SendWheel(terminal, 120, 120, expected_lines);
    TickAndPaint(terminal);
    const WxPaintMetrics started = terminal.GetPaintMetrics();
    if (started.wheel_lines_scrolled !=
            initial.wheel_lines_scrolled + expected_lines ||
        started.scroll_animation_starts !=
            initial.scroll_animation_starts + 1 ||
        started.scroll_animation_completions !=
            initial.scroll_animation_completions) {
        std::cerr << "wx wheel did not start the expected viewport scroll\n";
        return 7;
    }

    wxMilliSleep(10);
    TickAndPaint(terminal);
    const WxPaintMetrics mid_animation = terminal.GetPaintMetrics();
    const wxBitmap mid = CaptureClient(terminal);
    if (mid_animation.scroll_animation_paints <=
            started.scroll_animation_paints ||
        mid_animation.scroll_animation_completions !=
            initial.scroll_animation_completions ||
        DifferentPixels(before, mid) == 0) {
        std::cerr << "pixel animation was not visible in the wx client\n";
        return 8;
    }

    wxMilliSleep(120);
    TickAndPaint(terminal);
    const WxPaintMetrics completed = terminal.GetPaintMetrics();
    const wxBitmap after = CaptureClient(terminal);
    if (completed.scroll_animation_completions !=
            initial.scroll_animation_completions + 1 ||
        completed.scroll_animation_frames == initial.scroll_animation_frames) {
        std::cerr << "pixel scroll animation did not complete\n";
        return 9;
    }

    const wxSize cell_size = terminal.GetCellSize();
    const int expected_pixel_shift = expected_lines * cell_size.GetHeight();
    const ShiftMatch final_shift = BestVerticalShift(
        before, after, expected_pixel_shift + cell_size.GetHeight() * 2);
    if (final_shift.shift == 0 || final_shift.compared < 100 ||
        final_shift.matched * 100 < final_shift.compared * 80 ||
        std::abs(std::abs(final_shift.shift) - expected_pixel_shift) > 2) {
        std::cerr << "final wx pixels did not move by the expected number of rows\n";
        return 10;
    }

    return 0;
}

class ScrollTestApp final : public wxApp {
public:
    bool OnInit() override
    {
        result_ = RunScrollTest();
        if (result_ == 0)
            result_ = RunAnimationTest();
        CallAfter([this]() { ExitMainLoop(); });
        return true;
    }

    int OnExit() override
    {
        return result_;
    }

private:
    int result_ = 1;
};

} // namespace

wxIMPLEMENT_APP(ScrollTestApp);

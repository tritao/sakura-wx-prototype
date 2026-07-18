#include <sakura/wx/terminal_ctrl.h>

#include <tsm/libtsm.h>

#include <wx/clipbrd.h>
#include <wx/dataobj.h>
#include <wx/dcbuffer.h>
#include <wx/dcmemory.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iterator>
#include <list>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <xkbcommon/xkbcommon-keysyms.h>

namespace {

uint32_t ColorKey(const std::array<uint8_t, 3>& color)
{
    return (static_cast<uint32_t>(color[0]) << 16) |
           (static_cast<uint32_t>(color[1]) << 8) |
           static_cast<uint32_t>(color[2]);
}

std::string ToUtf8(const wxString& value)
{
    const auto utf8 = value.ToUTF8();
    if (utf8.data() == nullptr)
        return {};
    return {utf8.data(), utf8.length()};
}

wxFont CreateTerminalFont(const TerminalConfig& config)
{
    const int point_size = std::max(1, config.font_size);
    wxFontInfo info(point_size);
    info.Family(wxFONTFAMILY_TELETYPE);
    if (!config.use_system_font)
        info.FaceName(wxString::FromUTF8(config.font_family.c_str()));

    wxFont font(info);
    if (font.IsOk())
        return font;

    // Keep a usable monospace fallback if an explicitly requested face is
    // unavailable on the current platform.
    wxFontInfo fallback(point_size);
    fallback.Family(wxFONTFAMILY_TELETYPE);
    return wxFont(fallback);
}

void ScrollFramebuffer(wxMemoryDC& dc, int delta, unsigned int columns,
                       unsigned int rows, int cell_width, int cell_height)
{
    if (delta == 0 || columns == 0 || rows == 0 || cell_width <= 0 ||
        cell_height <= 0)
        return;
    const unsigned int distance = std::min(
        static_cast<unsigned int>(std::abs(delta)), rows);
    if (distance >= rows)
        return;

    const int width = static_cast<int>(columns) * cell_width;
    const int height = static_cast<int>(rows - distance) * cell_height;
    if (delta > 0) {
        dc.Blit(0, 0, width, height, &dc, 0,
                static_cast<int>(distance) * cell_height);
    } else {
        dc.Blit(0, static_cast<int>(distance) * cell_height, width, height,
                &dc, 0, 0);
    }
}

struct GlyphRunKey {
    std::string text;
    uint32_t foreground = 0;
    uint32_t background = 0;
    std::size_t font_identity = 0;
    unsigned int cell_count = 0;
    uint8_t attributes = 0;
    int cell_width = 0;
    int cell_height = 0;
    int dpi_x = 0;
    int dpi_y = 0;

    bool operator==(const GlyphRunKey& other) const
    {
        return text == other.text && foreground == other.foreground &&
            background == other.background &&
            font_identity == other.font_identity &&
            cell_count == other.cell_count && attributes == other.attributes &&
            cell_width == other.cell_width && cell_height == other.cell_height &&
            dpi_x == other.dpi_x && dpi_y == other.dpi_y;
    }
};

struct GlyphRunKeyHash {
    std::size_t operator()(const GlyphRunKey& key) const
    {
        std::size_t hash = std::hash<std::string>{}(key.text);
        const auto mix = [&hash](std::size_t value) {
            hash ^= value + static_cast<std::size_t>(0x9e3779b9) +
                (hash << 6) + (hash >> 2);
        };
        mix(key.foreground);
        mix(key.background);
        mix(key.font_identity);
        mix(key.cell_count);
        mix(key.attributes);
        mix(static_cast<std::size_t>(key.cell_width));
        mix(static_cast<std::size_t>(key.cell_height));
        mix(static_cast<std::size_t>(key.dpi_x));
        mix(static_cast<std::size_t>(key.dpi_y));
        return hash;
    }
};

struct GlyphRunResource {
    wxBitmap bitmap;
    wxSize measured_size;
};

struct GlyphRunCacheEntry {
    GlyphRunResource resource;
    std::size_t bytes = 0;
    std::list<GlyphRunKey>::iterator lru;
};

} // namespace

class WxTerminalCtrl::Impl {
public:
    Impl(WxTerminalCtrl& owner, std::unique_ptr<TerminalTransport> transport,
         TerminalConfig config, TerminalCallbacks callbacks)
        : owner_(owner),
          config_(std::move(config)),
          callbacks_(std::move(callbacks)),
          output_timer_(&owner_),
          transport_(std::move(transport))
    {
        core_ = sakura_terminal_new(&Impl::WriteBridge, this);
        const auto& background = config_.background;
        background_brush_ = wxBrush(wxColour(background[0], background[1],
                                              background[2]));
    }

    ~Impl()
    {
        sakura_terminal_frame_free(pending_frame_);
        sakura_terminal_free(core_);
    }

    static void WriteBridge(void* userdata, const char* data,
                            std::size_t length)
    {
        auto* impl = static_cast<Impl*>(userdata);
        if (impl != nullptr && impl->transport_ != nullptr)
            impl->transport_->Write(data, length);
    }

    void AssertOwnerThread() const
    {
        assert(std::this_thread::get_id() == owner_thread_);
    }

    static constexpr std::size_t kPaintSampleWindow = 256;

    static uint64_t PaintPercentile(
        const std::array<uint64_t, kPaintSampleWindow>& sorted,
        std::size_t count, std::size_t numerator, std::size_t denominator)
    {
        const std::size_t rank = std::max<std::size_t>(
            1, (count * numerator + denominator - 1) / denominator);
        return sorted[rank - 1];
    }

    void RecordPaintDuration(uint64_t elapsed_us)
    {
        paint_metrics_.paint_time_us += elapsed_us;
        paint_metrics_.max_paint_time_us = std::max(
            paint_metrics_.max_paint_time_us, elapsed_us);
        paint_time_samples_[paint_time_sample_cursor_] = elapsed_us;
        paint_time_sample_cursor_ =
            (paint_time_sample_cursor_ + 1) % kPaintSampleWindow;
        paint_time_sample_count_ = std::min(
            paint_time_sample_count_ + 1, kPaintSampleWindow);

        auto sorted = paint_time_samples_;
        std::sort(sorted.begin(), sorted.begin() +
                  static_cast<std::ptrdiff_t>(paint_time_sample_count_));
        paint_metrics_.p50_paint_time_us = PaintPercentile(
            sorted, paint_time_sample_count_, 50, 100);
        paint_metrics_.p95_paint_time_us = PaintPercentile(
            sorted, paint_time_sample_count_, 95, 100);
        paint_metrics_.p99_paint_time_us = PaintPercentile(
            sorted, paint_time_sample_count_, 99, 100);
    }

    using GlyphRunLru = std::list<GlyphRunKey>;
    using GlyphRunCache = std::unordered_map<
        GlyphRunKey, GlyphRunCacheEntry, GlyphRunKeyHash>;

    static std::size_t BitmapBytes(const wxBitmap& bitmap)
    {
        if (!bitmap.IsOk())
            return 0;
        const std::size_t width = static_cast<std::size_t>(
            std::max(0, bitmap.GetWidth()));
        const std::size_t height = static_cast<std::size_t>(
            std::max(0, bitmap.GetHeight()));
        const std::size_t depth = static_cast<std::size_t>(
            std::max(1, bitmap.GetDepth()));
        return width * height * ((depth + 7) / 8);
    }

    void UpdateGlyphCacheMetrics()
    {
        paint_metrics_.glyph_run_cache_entries =
            static_cast<uint64_t>(glyph_runs_.size());
        paint_metrics_.glyph_run_cache_bytes =
            static_cast<uint64_t>(glyph_run_cache_bytes_);
    }

    void ClearGlyphRunCache()
    {
        glyph_runs_.clear();
        glyph_run_lru_.clear();
        glyph_run_cache_bytes_ = 0;
        UpdateGlyphCacheMetrics();
    }

    void EvictLeastRecentlyUsedGlyphRun()
    {
        if (glyph_run_lru_.empty())
            return;
        const auto lru = std::prev(glyph_run_lru_.end());
        const auto cached = glyph_runs_.find(*lru);
        if (cached != glyph_runs_.end()) {
            glyph_run_cache_bytes_ -= cached->second.bytes;
            glyph_runs_.erase(cached);
        }
        glyph_run_lru_.erase(lru);
        ++paint_metrics_.glyph_run_cache_evictions;
        UpdateGlyphCacheMetrics();
    }

    struct ColorResources {
        wxColour color;
        wxBrush brush;
        wxPen pen;
    };

    const ColorResources& ColorResourcesFor(
        const std::array<uint8_t, 3>& color)
    {
        const uint32_t key = ColorKey(color);
        const auto existing = color_resources_.find(key);
        if (existing != color_resources_.end())
            return existing->second;
        if (color_resources_.size() >= 4096)
            color_resources_.clear();

        const wxColour wx_color(color[0], color[1], color[2]);
        const auto inserted = color_resources_.emplace(
            key, ColorResources {wx_color, wxBrush(wx_color), wxPen(wx_color)});
        return inserted.first->second;
    }

    WxTerminalCtrl& owner_;
    std::thread::id owner_thread_ = std::this_thread::get_id();
    TerminalConfig config_;
    TerminalCallbacks callbacks_;
    wxFont font_;
    std::array<wxFont, 8> glyph_fonts_;
    std::array<bool, 8> glyph_fonts_valid_ {};
    std::unordered_map<std::string, wxString> glyph_texts_;
    GlyphRunCache glyph_runs_;
    GlyphRunLru glyph_run_lru_;
    std::size_t glyph_run_cache_bytes_ = 0;
    wxBitmap glyph_bitmap_scratch_;
    std::size_t glyph_font_identity_ = 0;
    std::unordered_map<uint32_t, ColorResources> color_resources_;
    wxBrush background_brush_;
    wxBitmap framebuffer_;
    bool framebuffer_valid_ = false;
    SakuraTerminalFrame* pending_frame_ = nullptr;
    SakuraTerminalFrameInfo pending_info_ {};
    SakuraTerminalDirtyRegion pending_dirty_ {};
    bool pending_full_repaint_ = false;
    WxPaintMetrics paint_metrics_;
    std::array<uint64_t, kPaintSampleWindow> paint_time_samples_ {};
    std::size_t paint_time_sample_count_ = 0;
    std::size_t paint_time_sample_cursor_ = 0;
    wxTimer output_timer_;
    std::unique_ptr<TerminalTransport> transport_;
    SakuraTerminal* core_ = nullptr;
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
    unsigned int mouse_reporting_button_ = SAKURA_TERMINAL_MOUSE_LEFT;
    int auto_scroll_direction_ = 0;
    wxPoint last_pointer_position_;
    std::chrono::steady_clock::time_point last_click_time_;
    TransportState last_transport_state_ = TransportState::Stopped;
    std::chrono::steady_clock::time_point last_metrics_log_;
    std::chrono::steady_clock::time_point last_metrics_callback_;
    std::string last_title_;
    std::string last_error_;
};

WxTerminalCtrl::WxTerminalCtrl(
    wxWindow* parent, std::unique_ptr<TerminalTransport> transport,
    TerminalConfig config, TerminalCallbacks callbacks)
    : wxPanel(parent),
      impl_(std::make_unique<Impl>(*this, std::move(transport),
                                  std::move(config), std::move(callbacks)))
{
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    SetFocus();

    Bind(wxEVT_PAINT, &WxTerminalCtrl::OnPaint, this);
    Bind(wxEVT_SIZE, &WxTerminalCtrl::OnSize, this);
    Bind(wxEVT_CHAR, &WxTerminalCtrl::OnChar, this);
    Bind(wxEVT_MOUSEWHEEL, &WxTerminalCtrl::OnMouseWheel, this);
    Bind(wxEVT_LEFT_DOWN, &WxTerminalCtrl::OnLeftDown, this);
    Bind(wxEVT_LEFT_DCLICK, &WxTerminalCtrl::OnLeftDown, this);
    Bind(wxEVT_LEFT_UP, &WxTerminalCtrl::OnLeftUp, this);
    Bind(wxEVT_RIGHT_DOWN, &WxTerminalCtrl::OnMouseButtonDown, this);
    Bind(wxEVT_RIGHT_UP, &WxTerminalCtrl::OnMouseButtonUp, this);
    Bind(wxEVT_MIDDLE_DOWN, &WxTerminalCtrl::OnMouseButtonDown, this);
    Bind(wxEVT_MIDDLE_UP, &WxTerminalCtrl::OnMouseButtonUp, this);
    Bind(wxEVT_MOTION, &WxTerminalCtrl::OnMotion, this);
    Bind(wxEVT_TIMER, &WxTerminalCtrl::OnTimer, this);

    impl_->font_ = CreateTerminalFont(impl_->config_);
    impl_->glyph_font_identity_ = std::hash<std::string>{}(
        impl_->font_.GetNativeFontInfoDesc().ToStdString()) ^
        (std::hash<bool>{}(impl_->config_.use_system_font) << 1) ^
        (static_cast<std::size_t>(std::max(1, impl_->config_.font_size)) << 2);

    if (!sakura_terminal_is_ready(impl_->core_)) {
        impl_->error_ = wxString::FromUTF8(sakura_terminal_error(impl_->core_));
        ReportError(sakura_terminal_error(impl_->core_));
        return;
    }

    UpdateGeometry();
    const auto metrics_start = std::chrono::steady_clock::now();
    impl_->last_metrics_log_ = metrics_start;
    impl_->last_metrics_callback_ = metrics_start;
    if (impl_->transport_ != nullptr && impl_->config_.start_transport) {
        const char* shell = std::getenv("SHELL");
        if (!impl_->transport_->Start(impl_->columns_, impl_->rows_,
                                      shell == nullptr ? "" : shell)) {
            impl_->error_ = "No platform terminal transport is available";
            ReportError("No platform terminal transport is available");
            const char* notice =
                "\x1b[1;33mSakura wx terminal\x1b[0m\r\n"
                "This build has libtsm and wxWidgets, but could not start "
                "the platform process backend.\r\n";
            sakura_terminal_feed_output(impl_->core_, notice, std::strlen(notice));
        }
        const TransportStatus initial_status = impl_->transport_->GetStatus();
        impl_->last_transport_state_ = initial_status.state == TransportState::Failed
            ? TransportState::Failed
            : TransportState::Stopped;
        NotifyTitleChanged();
        NotifyTransportStatus(initial_status);
        impl_->trace_metrics_ = std::getenv("SAKURA_TRACE_METRICS") != nullptr;
    }
    impl_->output_timer_.Start(static_cast<int>(std::max(1u,
        impl_->config_.timer_interval_ms)));
}

WxTerminalCtrl::~WxTerminalCtrl()
{
    impl_->AssertOwnerThread();
    impl_->output_timer_.Stop();
    if (impl_->transport_ != nullptr)
        impl_->transport_->Stop();
}

SakuraTerminal* WxTerminalCtrl::Core()
{
    impl_->AssertOwnerThread();
    return impl_->core_;
}

const SakuraTerminal* WxTerminalCtrl::Core() const
{
    impl_->AssertOwnerThread();
    return impl_->core_;
}

std::string WxTerminalCtrl::GetFontFamily() const
{
    impl_->AssertOwnerThread();
    const std::string family = ToUtf8(impl_->font_.GetFaceName());
    if (!family.empty())
        return family;
    return impl_->config_.use_system_font ? "system monospace"
                                          : impl_->config_.font_family;
}

int WxTerminalCtrl::GetFontSize() const
{
    impl_->AssertOwnerThread();
    return impl_->font_.GetPointSize();
}

void WxTerminalCtrl::RefreshFrame()
{
    RequestFrameRefresh();
}

WxPaintMetrics WxTerminalCtrl::GetPaintMetrics() const
{
    impl_->AssertOwnerThread();
    return impl_->paint_metrics_;
}

void WxTerminalCtrl::RequestFrameRefresh()
{
    impl_->AssertOwnerThread();
    SakuraTerminalFrame* frame = sakura_terminal_take_frame(impl_->core_);
    SakuraTerminalFrameInfo info {};
    if (frame == nullptr || !sakura_terminal_frame_info(frame, &info)) {
        sakura_terminal_frame_free(frame);
        return;
    }
    if (!info.changed) {
        sakura_terminal_frame_free(frame);
        return;
    }

    SakuraTerminalDirtyRegion dirty = info.dirty;
    bool full_repaint = info.full_repaint != 0;
    int scroll_delta = info.scroll_delta;
    if (full_repaint)
        dirty = {0, 0, info.columns, info.rows};

    if (impl_->pending_frame_ != nullptr) {
        scroll_delta += impl_->pending_info_.scroll_delta;
        if (impl_->pending_full_repaint_)
            dirty = {0, 0, info.columns, info.rows};
        else if (dirty.left < dirty.right && dirty.top < dirty.bottom) {
            SakuraTerminalDirtyRegion& pending_dirty = impl_->pending_dirty_;
            if (pending_dirty.left < pending_dirty.right &&
                pending_dirty.top < pending_dirty.bottom) {
                dirty.left = std::min(dirty.left, pending_dirty.left);
                dirty.top = std::min(dirty.top, pending_dirty.top);
                dirty.right = std::max(dirty.right, pending_dirty.right);
                dirty.bottom = std::max(dirty.bottom, pending_dirty.bottom);
            }
        }
        full_repaint = full_repaint || impl_->pending_full_repaint_;
        sakura_terminal_frame_free(impl_->pending_frame_);
    }
    impl_->pending_frame_ = frame;
    impl_->pending_info_ = info;
    impl_->pending_info_.scroll_delta = scroll_delta;
    impl_->pending_full_repaint_ = full_repaint;
    impl_->pending_dirty_ = dirty;
    impl_->pending_info_.full_repaint = full_repaint ? 1 : 0;
    impl_->pending_info_.dirty = dirty;

    ++impl_->paint_metrics_.refresh_requests;
    const SakuraTerminalFrameInfo& pending = impl_->pending_info_;
    if (pending.full_repaint || pending.dirty.left >= pending.dirty.right ||
        pending.dirty.top >= pending.dirty.bottom) {
        ++impl_->paint_metrics_.full_refresh_requests;
        Refresh(false);
        return;
    }

    const unsigned int left = std::min(pending.dirty.left, pending.columns);
    const unsigned int top = std::min(pending.dirty.top, pending.rows);
    const unsigned int right = std::min(pending.dirty.right, pending.columns);
    const unsigned int bottom = std::min(pending.dirty.bottom, pending.rows);
    if (left >= right || top >= bottom) {
        ++impl_->paint_metrics_.full_refresh_requests;
        Refresh(false);
        return;
    }

    ++impl_->paint_metrics_.dirty_refresh_requests;
    RefreshRect(wxRect(left * impl_->cell_width_,
                       top * impl_->cell_height_,
                       (right - left) * impl_->cell_width_,
                       (bottom - top) * impl_->cell_height_), false);
}

void WxTerminalCtrl::UpdateGeometry()
{
    wxClientDC dc(this);
    dc.SetFont(impl_->font_);
    const int previous_cell_width = impl_->cell_width_;
    const int previous_cell_height = impl_->cell_height_;
    dc.GetTextExtent("M", &impl_->cell_width_, &impl_->cell_height_);
    impl_->cell_width_ = std::max(impl_->cell_width_, 1);
    impl_->cell_height_ = std::max(impl_->cell_height_, 1);
    if (previous_cell_width != impl_->cell_width_ ||
        previous_cell_height != impl_->cell_height_) {
        impl_->ClearGlyphRunCache();
        impl_->glyph_fonts_valid_.fill(false);
    }

    const wxSize size = GetClientSize();
    const unsigned int columns = static_cast<unsigned int>(
        std::max(1, size.GetWidth() / impl_->cell_width_));
    const unsigned int rows = static_cast<unsigned int>(
        std::max(1, size.GetHeight() / impl_->cell_height_));

    if (columns == impl_->columns_ && rows == impl_->rows_)
        return;
    impl_->columns_ = columns;
    impl_->rows_ = rows;
    sakura_terminal_resize(impl_->core_, impl_->columns_, impl_->rows_);
    if (impl_->transport_ != nullptr)
        impl_->transport_->Resize(impl_->columns_, impl_->rows_);
}

uint32_t WxTerminalCtrl::KeySymFor(const wxKeyEvent& event)
{
    switch (event.GetKeyCode()) {
    case WXK_BACK: return XKB_KEY_BackSpace;
    case WXK_TAB: return XKB_KEY_Tab;
    case WXK_RETURN:
    case WXK_NUMPAD_ENTER: return XKB_KEY_Return;
    case WXK_ESCAPE: return XKB_KEY_Escape;
    case WXK_DELETE: return XKB_KEY_Delete;
    case WXK_INSERT: return XKB_KEY_Insert;
    case WXK_HOME: return XKB_KEY_Home;
    case WXK_END: return XKB_KEY_End;
    case WXK_PAGEUP: return XKB_KEY_Page_Up;
    case WXK_PAGEDOWN: return XKB_KEY_Page_Down;
    case WXK_LEFT: return XKB_KEY_Left;
    case WXK_RIGHT: return XKB_KEY_Right;
    case WXK_UP: return XKB_KEY_Up;
    case WXK_DOWN: return XKB_KEY_Down;
    default: break;
    }

    const int key = event.GetKeyCode();
    if (key >= WXK_F1 && key <= WXK_F12)
        return XKB_KEY_F1 + static_cast<uint32_t>(key - WXK_F1);
    return event.GetUnicodeKey();
}

const wxFont& WxTerminalCtrl::GlyphFont(uint8_t attributes)
{
    const std::size_t index = attributes & 0x07;
    if (!impl_->glyph_fonts_valid_[index]) {
        wxFont& glyph_font = impl_->glyph_fonts_[index];
        glyph_font = impl_->font_;
        glyph_font.SetWeight((attributes & 0x01) != 0
            ? wxFONTWEIGHT_BOLD : wxFONTWEIGHT_NORMAL);
        glyph_font.SetStyle((attributes & 0x02) != 0
            ? wxFONTSTYLE_ITALIC : wxFONTSTYLE_NORMAL);
        glyph_font.SetUnderlined((attributes & 0x04) != 0);
        impl_->glyph_fonts_valid_[index] = true;
    }
    return impl_->glyph_fonts_[index];
}

const wxString& WxTerminalCtrl::GlyphText(const char* text, std::size_t length)
{
    constexpr std::size_t max_glyph_texts = 4096;
    const std::string key(text == nullptr ? "" : std::string(text, length));
    const auto existing = impl_->glyph_texts_.find(key);
    if (existing != impl_->glyph_texts_.end())
        return existing->second;
    if (impl_->glyph_texts_.size() >= max_glyph_texts)
        impl_->glyph_texts_.clear();
    const auto inserted = impl_->glyph_texts_.emplace(
        key, wxString::FromUTF8(key.c_str()));
    return inserted.first->second;
}

const wxBitmap& WxTerminalCtrl::GlyphRunBitmap(
    wxDC& dc, const SakuraTerminalRunView& run, const wxString& text,
    const std::array<uint8_t, 3>& foreground,
    const std::array<uint8_t, 3>& background)
{
    GlyphRunKey key;
    key.text.assign(run.text == nullptr ? "" : run.text, run.text_length);
    key.foreground = ColorKey(foreground);
    key.background = ColorKey(background);
    key.font_identity = impl_->glyph_font_identity_;
    key.cell_count = run.cell_count;
    key.attributes = run.attributes;
    key.cell_width = impl_->cell_width_;
    key.cell_height = impl_->cell_height_;
    const wxSize ppi = dc.GetPPI();
    key.dpi_x = ppi.GetWidth();
    key.dpi_y = ppi.GetHeight();

    const auto existing = impl_->glyph_runs_.find(key);
    if (existing != impl_->glyph_runs_.end()) {
        impl_->glyph_run_lru_.splice(impl_->glyph_run_lru_.begin(),
                                     impl_->glyph_run_lru_,
                                     existing->second.lru);
        ++impl_->paint_metrics_.glyph_run_cache_hits;
        return existing->second.resource.bitmap;
    }

    ++impl_->paint_metrics_.glyph_run_cache_misses;

    const wxFont& font = GlyphFont(run.attributes);
    dc.SetFont(font);
    ++impl_->paint_metrics_.dc_state_changes;
    const wxSize measured_size = dc.GetTextExtent(text);
    const int run_width = static_cast<int>(run.cell_count) *
        impl_->cell_width_;
    const int bitmap_width = std::max(1, std::max(run_width,
                                                  measured_size.GetWidth()));
    const int bitmap_height = std::max(1, impl_->cell_height_);

    wxBitmap bitmap(bitmap_width, bitmap_height, -1);
    if (bitmap.IsOk()) {
        wxMemoryDC glyph_dc(bitmap);
        glyph_dc.SetBackground(wxBrush(wxColour(background[0], background[1],
                                                 background[2])));
        glyph_dc.Clear();
        glyph_dc.SetTextForeground(wxColour(foreground[0], foreground[1],
                                            foreground[2]));
        glyph_dc.SetFont(font);
        glyph_dc.DrawText(text, 0, 0);
        glyph_dc.SelectObject(wxNullBitmap);
    }

    const std::size_t bitmap_bytes = Impl::BitmapBytes(bitmap);
    const bool cacheable = bitmap.IsOk() &&
        impl_->config_.glyph_cache_max_entries > 0 &&
        bitmap_bytes <= impl_->config_.glyph_cache_max_bytes;
    if (!cacheable) {
        impl_->glyph_bitmap_scratch_ = bitmap;
        return impl_->glyph_bitmap_scratch_;
    }

    while (!impl_->glyph_run_lru_.empty() &&
           (impl_->glyph_runs_.size() >=
                impl_->config_.glyph_cache_max_entries ||
            impl_->glyph_run_cache_bytes_ >
                impl_->config_.glyph_cache_max_bytes - bitmap_bytes))
        impl_->EvictLeastRecentlyUsedGlyphRun();

    auto inserted = impl_->glyph_runs_.emplace(
        std::move(key), GlyphRunCacheEntry {});
    inserted.first->second.resource.bitmap = bitmap;
    inserted.first->second.resource.measured_size = measured_size;
    inserted.first->second.bytes = bitmap_bytes;
    impl_->glyph_run_lru_.push_front(inserted.first->first);
    inserted.first->second.lru = impl_->glyph_run_lru_.begin();
    impl_->glyph_run_cache_bytes_ += bitmap_bytes;
    impl_->paint_metrics_.glyph_run_cache_peak_bytes = std::max(
        impl_->paint_metrics_.glyph_run_cache_peak_bytes,
        static_cast<uint64_t>(impl_->glyph_run_cache_bytes_));
    impl_->UpdateGlyphCacheMetrics();
    return inserted.first->second.resource.bitmap;
}

void WxTerminalCtrl::RenderFrame(wxDC& dc,
                                 const SakuraTerminalFrame* frame,
                                 const SakuraTerminalFrameInfo& info,
                                 const SakuraTerminalDirtyRegion& dirty,
                                 uint64_t* painted_cells)
{
    if (frame == nullptr || dirty.left >= dirty.right ||
        dirty.top >= dirty.bottom || info.columns == 0 || info.rows == 0)
        return;

    const unsigned int left = std::min(dirty.left, info.columns);
    const unsigned int top = std::min(dirty.top, info.rows);
    const unsigned int right = std::min(dirty.right, info.columns);
    const unsigned int bottom = std::min(dirty.bottom, info.rows);
    if (left >= right || top >= bottom)
        return;

    dc.SetPen(*wxTRANSPARENT_PEN);
    ++impl_->paint_metrics_.dc_state_changes;
    dc.SetBrush(impl_->background_brush_);
    ++impl_->paint_metrics_.dc_state_changes;
    dc.DrawRectangle(left * impl_->cell_width_, top * impl_->cell_height_,
                     (right - left) * impl_->cell_width_,
                     (bottom - top) * impl_->cell_height_);
    ++impl_->paint_metrics_.background_rectangles;

    const uint32_t default_background = ColorKey(impl_->config_.background);
    const bool cache_glyphs = impl_->config_.glyph_cache_enabled &&
        (!impl_->config_.glyph_cache_bypass_scroll || info.scroll_delta == 0);

    for (unsigned int row = top; row < bottom; ++row) {
        const std::size_t run_count =
            sakura_terminal_frame_row_run_count(frame, row);
        std::vector<SakuraTerminalRunView> row_runs;
        row_runs.reserve(run_count);
        for (std::size_t index = 0; index < run_count; ++index) {
            SakuraTerminalRunView run {};
            if (sakura_terminal_frame_row_run(frame, row, index, &run))
                row_runs.push_back(run);
        }

        bool pending_background = false;
        uint32_t pending_background_key = 0;
        std::array<uint8_t, 3> pending_background_color {};
        unsigned int pending_background_left = 0;
        unsigned int pending_background_right = 0;
        const auto flush_background = [&]() {
            if (!pending_background)
                return;
            dc.SetBrush(impl_->ColorResourcesFor(
                pending_background_color).brush);
            ++impl_->paint_metrics_.dc_state_changes;
            dc.DrawRectangle(
                pending_background_left * impl_->cell_width_,
                row * impl_->cell_height_,
                (pending_background_right - pending_background_left) *
                    impl_->cell_width_,
                impl_->cell_height_);
            ++impl_->paint_metrics_.background_rectangles;
            pending_background = false;
        };

        for (const SakuraTerminalRunView& run : row_runs) {

            const unsigned int run_right = std::min(
                info.columns, run.left + std::max(1u, run.cell_count));
            const unsigned int draw_left = std::max(left, run.left);
            const unsigned int draw_right = std::min(right, run_right);
            if (draw_left >= draw_right)
                continue;

            const std::array<uint8_t, 3> background {
                run.background[0], run.background[1], run.background[2]};
            const uint32_t background_key = ColorKey(background);
            if (background_key == default_background) {
                flush_background();
                continue;
            }
            if (!pending_background ||
                pending_background_key != background_key ||
                pending_background_right != draw_left) {
                flush_background();
                pending_background = true;
                pending_background_key = background_key;
                pending_background_color = background;
                pending_background_left = draw_left;
                pending_background_right = draw_right;
            } else {
                pending_background_right = draw_right;
            }
        }
        flush_background();

        for (const SakuraTerminalRunView& run : row_runs) {
            const unsigned int run_right = std::min(
                info.columns, run.left + std::max(1u, run.cell_count));
            const unsigned int draw_left = std::max(left, run.left);
            const unsigned int draw_right = std::min(right, run_right);
            if (draw_left >= draw_right)
                continue;
            if (painted_cells != nullptr)
                *painted_cells += draw_right - draw_left;

            const std::array<uint8_t, 3> foreground {
                run.foreground[0], run.foreground[1], run.foreground[2]};
            const std::array<uint8_t, 3> background {
                run.background[0], run.background[1], run.background[2]};

            const auto draw_glyph_run = [&](const SakuraTerminalRunView& glyph_run,
                                            const wxString& glyph) {
                if (cache_glyphs) {
                    const wxBitmap& glyph_bitmap = GlyphRunBitmap(
                        dc, glyph_run, glyph, foreground, background);
                    if (glyph_bitmap.IsOk()) {
                        dc.DrawBitmap(glyph_bitmap,
                                      glyph_run.left * impl_->cell_width_,
                                      row * impl_->cell_height_, false);
                        ++impl_->paint_metrics_.glyph_bitmap_draws;
                    } else {
                        dc.SetTextForeground(
                            impl_->ColorResourcesFor(foreground).color);
                        ++impl_->paint_metrics_.dc_state_changes;
                        dc.SetFont(GlyphFont(glyph_run.attributes));
                        ++impl_->paint_metrics_.dc_state_changes;
                        dc.DrawText(glyph,
                                    glyph_run.left * impl_->cell_width_,
                                    row * impl_->cell_height_);
                        ++impl_->paint_metrics_.glyph_text_draws;
                    }
                } else {
                    if (impl_->config_.glyph_cache_enabled &&
                        impl_->config_.glyph_cache_bypass_scroll &&
                        info.scroll_delta != 0)
                        ++impl_->paint_metrics_.glyph_run_cache_bypasses;
                    dc.SetTextForeground(
                        impl_->ColorResourcesFor(foreground).color);
                    ++impl_->paint_metrics_.dc_state_changes;
                    dc.SetFont(GlyphFont(glyph_run.attributes));
                    ++impl_->paint_metrics_.dc_state_changes;
                    dc.DrawText(glyph,
                                glyph_run.left * impl_->cell_width_,
                                row * impl_->cell_height_);
                    ++impl_->paint_metrics_.glyph_text_draws;
                }
            };

            const auto draw_text_run = [&](const SakuraTerminalRunView& text_run) {
                if (text_run.text == nullptr || text_run.text_length == 0)
                    return;
                bool has_glyph = false;
                for (std::size_t text_index = 0;
                     text_index < text_run.text_length; ++text_index) {
                    if (text_run.text[text_index] != ' ') {
                        has_glyph = true;
                        break;
                    }
                }
                if (!has_glyph || text_run.text_length == 0)
                    return;
                const wxString& glyph = GlyphText(
                    text_run.text, text_run.text_length);
                if (!glyph.empty()) {
                    ++impl_->paint_metrics_.glyph_run_spans;
                    draw_glyph_run(text_run, glyph);
                }
            };
            draw_text_run(run);
            if ((run.attributes & 0x04) != 0) {
                dc.SetPen(impl_->ColorResourcesFor(foreground).pen);
                ++impl_->paint_metrics_.dc_state_changes;
                const int underline_y = (row + 1) * impl_->cell_height_ - 2;
                dc.DrawLine(draw_left * impl_->cell_width_, underline_y,
                            draw_right * impl_->cell_width_ - 1, underline_y);
                dc.SetPen(*wxTRANSPARENT_PEN);
                ++impl_->paint_metrics_.dc_state_changes;
            }
        }
    }

    const bool cursor_in_dirty_region =
        info.cursor_x >= left && info.cursor_x < right &&
        info.cursor_y >= top && info.cursor_y < bottom;
    if (cursor_in_dirty_region && info.cursor_visible &&
        info.cursor_x < info.columns && info.cursor_y < info.rows) {
        dc.SetBrush(*wxTRANSPARENT_BRUSH);
        ++impl_->paint_metrics_.dc_state_changes;
        dc.SetPen(wxPen(wxColour(230, 230, 230), 1));
        ++impl_->paint_metrics_.dc_state_changes;
        const int cursor_x = info.cursor_x * impl_->cell_width_;
        const int cursor_y = info.cursor_y * impl_->cell_height_;
        switch (info.cursor_style) {
        case SAKURA_TERMINAL_CURSOR_UNDERLINE:
            dc.DrawLine(cursor_x, cursor_y + impl_->cell_height_ - 2,
                        cursor_x + impl_->cell_width_ - 1,
                        cursor_y + impl_->cell_height_ - 2);
            break;
        case SAKURA_TERMINAL_CURSOR_BAR:
            dc.DrawLine(cursor_x + 1, cursor_y + 1,
                        cursor_x + 1, cursor_y + impl_->cell_height_ - 2);
            break;
        case SAKURA_TERMINAL_CURSOR_BLOCK:
            dc.DrawRectangle(cursor_x, cursor_y, impl_->cell_width_, impl_->cell_height_);
            break;
        }
    }
}

void WxTerminalCtrl::OnPaint(wxPaintEvent&)
{
    const auto paint_start = std::chrono::steady_clock::now();
    ++impl_->paint_metrics_.paint_events;
    const auto record_paint = [this, &paint_start]() {
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - paint_start).count();
        const uint64_t elapsed_us = static_cast<uint64_t>(
            std::max<int64_t>(0, elapsed));
        impl_->RecordPaintDuration(elapsed_us);
    };
    wxAutoBufferedPaintDC dc(this);
    const auto& background = impl_->config_.background;
    dc.SetBackground(wxBrush(wxColour(background[0], background[1],
                                       background[2])));

    if (!sakura_terminal_is_ready(impl_->core_)) {
        dc.Clear();
        const auto& error_foreground = impl_->config_.error_foreground;
        dc.SetTextForeground(wxColour(error_foreground[0], error_foreground[1],
                                      error_foreground[2]));
        dc.DrawText(impl_->error_, 12, 12);
        record_paint();
        return;
    }

    SakuraTerminalFrame* frame = nullptr;
    SakuraTerminalFrameInfo info {};
    SakuraTerminalDirtyRegion dirty {};
    bool full_repaint = false;
    if (impl_->pending_frame_ != nullptr) {
        frame = impl_->pending_frame_;
        info = impl_->pending_info_;
        dirty = impl_->pending_dirty_;
        full_repaint = impl_->pending_full_repaint_;
        impl_->pending_frame_ = nullptr;
        impl_->pending_info_ = {};
        impl_->pending_dirty_ = {};
        impl_->pending_full_repaint_ = false;
    } else {
        frame = sakura_terminal_take_frame(impl_->core_);
        if (frame == nullptr || !sakura_terminal_frame_info(frame, &info)) {
            sakura_terminal_frame_free(frame);
            record_paint();
            return;
        }
        dirty = info.dirty;
        full_repaint = info.full_repaint != 0;
    }
    if (frame == nullptr) {
        record_paint();
        return;
    }
    const wxSize client_size = GetClientSize();
    const int bitmap_width = std::max(1, client_size.GetWidth());
    const int bitmap_height = std::max(1, client_size.GetHeight());
    if (!impl_->framebuffer_.IsOk() ||
        impl_->framebuffer_.GetWidth() != bitmap_width ||
        impl_->framebuffer_.GetHeight() != bitmap_height) {
        impl_->framebuffer_ = wxBitmap(bitmap_width, bitmap_height, -1);
        impl_->framebuffer_valid_ = false;
        ++impl_->paint_metrics_.framebuffer_rebuilds;
    }

    wxMemoryDC framebuffer_dc;
    framebuffer_dc.SelectObject(impl_->framebuffer_);
    uint64_t painted_cells = 0;
    if (!impl_->framebuffer_valid_ || full_repaint) {
        ++impl_->paint_metrics_.full_repaints;
        framebuffer_dc.SetBackground(wxBrush(wxColour(background[0],
                                                       background[1],
                                                       background[2])));
        framebuffer_dc.Clear();
        const SakuraTerminalDirtyRegion full_region {
            0, 0, info.columns, info.rows
        };
        RenderFrame(framebuffer_dc, frame, info, full_region, &painted_cells);
        impl_->framebuffer_valid_ = true;
    } else if (info.changed) {
        if (info.scroll_delta != 0)
            ScrollFramebuffer(framebuffer_dc, info.scroll_delta, info.columns,
                              info.rows, impl_->cell_width_,
                              impl_->cell_height_);
        ++impl_->paint_metrics_.partial_repaints;
        RenderFrame(framebuffer_dc, frame, info, dirty, &painted_cells);
    }
    framebuffer_dc.SelectObject(wxNullBitmap);
    impl_->paint_metrics_.painted_cells += painted_cells;

    dc.DrawBitmap(impl_->framebuffer_, 0, 0, false);
    sakura_terminal_frame_free(frame);
    record_paint();
}

void WxTerminalCtrl::OnSize(wxSizeEvent& event)
{
    UpdateGeometry();
    ++impl_->paint_metrics_.refresh_requests;
    ++impl_->paint_metrics_.full_refresh_requests;
    Refresh(false);
    event.Skip();
}

void WxTerminalCtrl::OnChar(wxKeyEvent& event)
{
    const uint32_t key = static_cast<uint32_t>(event.GetKeyCode());
    const uint32_t unicode = event.GetUnicodeKey();
    const bool copy_key = key == 'c' || key == 'C' || unicode == 'c' ||
                          unicode == 'C' || key == 3;
    const bool paste_key = key == 'v' || key == 'V' || unicode == 'v' ||
                           unicode == 'V' || key == 22;
    const bool restart_key = key == 'r' || key == 'R' || unicode == 'r' ||
                             unicode == 'R' || key == 18;
    if ((event.ControlDown() || event.MetaDown()) && event.ShiftDown()) {
        if (restart_key) {
            RestartTransport();
            return;
        }
        if (copy_key) {
            CopySelectionToClipboard();
            return;
        }
        if (paste_key) {
            PasteFromClipboard();
            return;
        }
    }

    unsigned int modifiers = 0;
    if (event.ShiftDown()) modifiers |= SAKURA_TERMINAL_SHIFT;
    if (event.ControlDown()) modifiers |= SAKURA_TERMINAL_CONTROL;
    if (event.AltDown()) modifiers |= SAKURA_TERMINAL_ALT;
    if (event.MetaDown()) modifiers |= SAKURA_TERMINAL_LOGO;

    const uint32_t ascii = unicode <= 0x7f ? unicode : SAKURA_TERMINAL_INVALID;
    if (sakura_terminal_handle_key(impl_->core_, KeySymFor(event), ascii,
                                   modifiers, unicode)) {
        RequestFrameRefresh();
        return;
    }
    event.Skip();
}

void WxTerminalCtrl::OnMouseWheel(wxMouseEvent& event)
{
    if (!event.ShiftDown() &&
        sakura_terminal_mouse_reporting_enabled(impl_->core_)) {
        const unsigned int button = event.GetWheelRotation() > 0
            ? SAKURA_TERMINAL_MOUSE_WHEEL_UP
            : SAKURA_TERMINAL_MOUSE_WHEEL_DOWN;
        const auto [column, row] = CellAt(event.GetPosition());
        sakura_terminal_handle_mouse(
            impl_->core_, column, row,
            static_cast<unsigned int>(std::max(0, event.GetX())),
            static_cast<unsigned int>(std::max(0, event.GetY())), button,
            SAKURA_TERMINAL_MOUSE_PRESSED, MouseModifiers(event));
        return;
    }
    if (event.GetWheelRotation() > 0)
        sakura_terminal_scroll_page_up(impl_->core_, 3);
    else if (event.GetWheelRotation() < 0)
        sakura_terminal_scroll_page_down(impl_->core_, 3);
    RequestFrameRefresh();
}

std::pair<unsigned int, unsigned int>
WxTerminalCtrl::CellAt(const wxPoint& point) const
{
    const int column = std::max(0, point.x) / std::max(1, impl_->cell_width_);
    const int row = std::max(0, point.y) / std::max(1, impl_->cell_height_);
    return {
        std::min(static_cast<unsigned int>(column), impl_->columns_ - 1),
        std::min(static_cast<unsigned int>(row), impl_->rows_ - 1),
    };
}

unsigned char WxTerminalCtrl::MouseModifiers(const wxMouseEvent& event)
{
    unsigned char modifiers = 0;
    if (event.ShiftDown()) modifiers |= SAKURA_TERMINAL_MOUSE_SHIFT;
    if (event.MetaDown()) modifiers |= SAKURA_TERMINAL_MOUSE_META;
    if (event.ControlDown()) modifiers |= SAKURA_TERMINAL_MOUSE_CONTROL;
    return modifiers;
}

bool WxTerminalCtrl::ForwardMouse(const wxMouseEvent& event,
                                  unsigned int button,
                                  unsigned int mouse_event)
{
    if (event.ShiftDown() ||
        !sakura_terminal_mouse_reporting_enabled(impl_->core_))
        return false;
    const auto [column, row] = CellAt(event.GetPosition());
    return sakura_terminal_handle_mouse(
        impl_->core_, column, row,
        static_cast<unsigned int>(std::max(0, event.GetX())),
        static_cast<unsigned int>(std::max(0, event.GetY())),
        button, mouse_event, MouseModifiers(event));
}

void WxTerminalCtrl::BeginMouseReporting(unsigned int button)
{
    impl_->mouse_reporting_gesture_ = true;
    impl_->mouse_reporting_button_ = button;
    CaptureMouse();
}

void WxTerminalCtrl::EndMouseReporting(const wxMouseEvent& event)
{
    if (!impl_->mouse_reporting_gesture_)
        return;
    const auto [column, row] = CellAt(event.GetPosition());
    sakura_terminal_handle_mouse(
        impl_->core_, column, row,
        static_cast<unsigned int>(std::max(0, event.GetX())),
        static_cast<unsigned int>(std::max(0, event.GetY())),
        impl_->mouse_reporting_button_, SAKURA_TERMINAL_MOUSE_RELEASED,
        MouseModifiers(event));
    impl_->mouse_reporting_gesture_ = false;
    if (HasCapture())
        ReleaseMouse();
}

void WxTerminalCtrl::OnLeftDown(wxMouseEvent& event)
{
    SetFocus();
    const auto [column, row] = CellAt(event.GetPosition());
    if (ForwardMouse(event, SAKURA_TERMINAL_MOUSE_LEFT,
                     SAKURA_TERMINAL_MOUSE_PRESSED)) {
        BeginMouseReporting(SAKURA_TERMINAL_MOUSE_LEFT);
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    const bool same_cell = column == impl_->last_click_column_ &&
                           row == impl_->last_click_row_;
    const bool quick_click = now - impl_->last_click_time_ <
                             std::chrono::milliseconds(500);
    if (event.GetClickCount() >= 2 || (same_cell && quick_click))
        impl_->click_count_ = impl_->click_count_ % 3 + 1;
    else
        impl_->click_count_ = 1;
    impl_->last_click_time_ = now;
    impl_->last_click_column_ = column;
    impl_->last_click_row_ = row;

    const bool extend_selection = event.ShiftDown() &&
        sakura_terminal_has_selection(impl_->core_);
    if (extend_selection) {
        sakura_terminal_update_selection(impl_->core_, column, row);
    } else {
        sakura_terminal_clear_selection(impl_->core_);
        if (impl_->click_count_ == 2)
            sakura_terminal_select_word(impl_->core_, column, row);
        else if (impl_->click_count_ == 3)
            sakura_terminal_select_line(impl_->core_, row);
    }

    impl_->pointer_down_ = true;
    impl_->selection_dragging_ = extend_selection;
    impl_->pointer_down_position_ = event.GetPosition();
    impl_->selection_anchor_column_ = column;
    impl_->selection_anchor_row_ = row;
    CaptureMouse();
    RequestFrameRefresh();
}

void WxTerminalCtrl::OnLeftUp(wxMouseEvent& event)
{
    if (impl_->mouse_reporting_gesture_) {
        EndMouseReporting(event);
        return;
    }
    if (!impl_->pointer_down_)
        return;
    const auto [column, row] = CellAt(event.GetPosition());
    const bool should_copy = impl_->selection_dragging_ || impl_->click_count_ >= 2;
    if (impl_->selection_dragging_)
        sakura_terminal_update_selection(impl_->core_, column, row);
    impl_->selection_dragging_ = false;
    impl_->pointer_down_ = false;
    impl_->auto_scroll_direction_ = 0;
    if (HasCapture())
        ReleaseMouse();
    if (should_copy)
        CopySelectionToClipboard();
    RequestFrameRefresh();
}

void WxTerminalCtrl::OnMouseButtonDown(wxMouseEvent& event)
{
    const unsigned int button = event.RightDown()
        ? SAKURA_TERMINAL_MOUSE_RIGHT
        : SAKURA_TERMINAL_MOUSE_MIDDLE;
    if (ForwardMouse(event, button, SAKURA_TERMINAL_MOUSE_PRESSED))
        BeginMouseReporting(button);
}

void WxTerminalCtrl::OnMouseButtonUp(wxMouseEvent& event)
{
    if (impl_->mouse_reporting_gesture_)
        EndMouseReporting(event);
}

void WxTerminalCtrl::UpdateSelectionAt(const wxPoint& position)
{
    impl_->auto_scroll_direction_ = 0;
    if (position.y < 0)
        impl_->auto_scroll_direction_ = -1;
    else if (position.y >= GetClientSize().GetHeight())
        impl_->auto_scroll_direction_ = 1;

    if (impl_->auto_scroll_direction_ != 0)
        sakura_terminal_scroll_lines(impl_->core_,
                                     impl_->auto_scroll_direction_ < 0 ? 1 : -1);

    const auto [column, row] = CellAt(position);
    sakura_terminal_update_selection(impl_->core_, column, row);
}

void WxTerminalCtrl::OnMotion(wxMouseEvent& event)
{
    if (impl_->mouse_reporting_gesture_) {
        if (event.Dragging()) {
            const auto [column, row] = CellAt(event.GetPosition());
            sakura_terminal_handle_mouse(
                impl_->core_, column, row,
                static_cast<unsigned int>(std::max(0, event.GetX())),
                static_cast<unsigned int>(std::max(0, event.GetY())),
                32 + impl_->mouse_reporting_button_, SAKURA_TERMINAL_MOUSE_MOVED,
                MouseModifiers(event));
        }
        return;
    }
    if (!impl_->pointer_down_ || !event.Dragging()) {
        if (!event.ShiftDown() &&
            sakura_terminal_mouse_reporting_enabled(impl_->core_)) {
            const auto [column, row] = CellAt(event.GetPosition());
            sakura_terminal_handle_mouse(
                impl_->core_, column, row,
                static_cast<unsigned int>(std::max(0, event.GetX())),
                static_cast<unsigned int>(std::max(0, event.GetY())),
                SAKURA_TERMINAL_MOUSE_LEFT, SAKURA_TERMINAL_MOUSE_MOVED,
                MouseModifiers(event));
        }
        return;
    }
    const wxPoint position = event.GetPosition();
    const int distance_x = std::abs(position.x - impl_->pointer_down_position_.x);
    const int distance_y = std::abs(position.y - impl_->pointer_down_position_.y);
    if (!impl_->selection_dragging_ && (distance_x >= 4 || distance_y >= 4)) {
        sakura_terminal_start_selection(impl_->core_,
                                        impl_->selection_anchor_column_,
                                        impl_->selection_anchor_row_);
        impl_->selection_dragging_ = true;
    }
    if (!impl_->selection_dragging_)
        return;
    impl_->last_pointer_position_ = position;
    UpdateSelectionAt(position);
    RequestFrameRefresh();
}

bool WxTerminalCtrl::CopySelectionToClipboard()
{
    char* copied_text = sakura_terminal_copy_selection(impl_->core_);
    if (copied_text == nullptr || *copied_text == '\0' ||
        wxTheClipboard == nullptr || !wxTheClipboard->Open()) {
        sakura_terminal_free_string(copied_text);
        return false;
    }
    const std::string text(copied_text);
    sakura_terminal_free_string(copied_text);
    const bool copied = wxTheClipboard->SetData(
        new wxTextDataObject(wxString::FromUTF8(text)));
    wxTheClipboard->Close();
    return copied;
}

bool WxTerminalCtrl::PasteFromClipboard()
{
    if (wxTheClipboard == nullptr || !wxTheClipboard->Open())
        return false;
    wxTextDataObject data;
    const bool available = wxTheClipboard->GetData(data);
    wxTheClipboard->Close();
    if (!available)
        return false;

    const wxString text = data.GetText();
    const auto utf8 = text.ToUTF8();
    if (utf8.data() == nullptr)
        return false;
    sakura_terminal_paste(impl_->core_, utf8.data(), utf8.length());
    RequestFrameRefresh();
    return true;
}

void WxTerminalCtrl::NotifyTitleChanged()
{
    impl_->AssertOwnerThread();
    const char* title_value = sakura_terminal_title(impl_->core_);
    const std::string title = title_value == nullptr ? "" : title_value;
    if (title == impl_->last_title_)
        return;
    impl_->last_title_ = title;
    if (impl_->callbacks_.on_title_changed)
        impl_->callbacks_.on_title_changed(title);
}

void WxTerminalCtrl::NotifyTransportStatus(const TransportStatus& status)
{
    impl_->AssertOwnerThread();
    if (impl_->callbacks_.on_transport_status_changed)
        impl_->callbacks_.on_transport_status_changed(status);
}

void WxTerminalCtrl::ReportError(const std::string& message)
{
    impl_->AssertOwnerThread();
    if (message.empty() || message == impl_->last_error_)
        return;
    impl_->last_error_ = message;
    if (impl_->callbacks_.on_error)
        impl_->callbacks_.on_error(message);
}

void WxTerminalCtrl::ShowTransportNotice(const TransportStatus& status)
{
    std::string notice = "\r\n\x1b[1;33m[terminal process ";
    switch (status.state) {
    case TransportState::Running:
        notice += "started";
        break;
    case TransportState::Exited:
        notice += "exited";
        if (status.exit_code_valid && status.signal == 0)
            notice += " with code " + std::to_string(status.exit_code);
        else if (status.exit_code_valid && status.signal != 0)
            notice += " on signal " + std::to_string(status.signal);
        break;
    case TransportState::Failed:
        notice += "failed to start";
        break;
    case TransportState::Starting:
        notice += "starting";
        break;
    case TransportState::Stopped:
        notice += "stopped";
        break;
    }
    notice += "]\x1b[0m\r\n";
    sakura_terminal_feed_output(impl_->core_, notice.data(), notice.size());
}

void WxTerminalCtrl::RestartTransport()
{
    if (impl_->transport_ == nullptr)
        return;
    impl_->transport_->Stop();
    sakura_terminal_clear_selection(impl_->core_);
    const char* shell = std::getenv("SHELL");
    impl_->transport_->Start(impl_->columns_, impl_->rows_, shell == nullptr ? "" : shell);
    const char* clear = "\x1b[2J\x1b[H";
    sakura_terminal_feed_output(impl_->core_, clear, std::strlen(clear));
    const TransportStatus status = impl_->transport_->GetStatus();
    ShowTransportNotice(status);
    impl_->last_transport_state_ = status.state == TransportState::Failed
        ? TransportState::Failed
        : TransportState::Stopped;
    NotifyTransportStatus(status);
    if (status.state == TransportState::Failed)
        ReportError("Terminal process failed to start");
    RequestFrameRefresh();
}

void WxTerminalCtrl::UpdateTransportStatus()
{
    if (impl_->transport_ == nullptr)
        return;
    const TransportStatus status = impl_->transport_->GetStatus();
    if (status.state == impl_->last_transport_state_)
        return;
    impl_->last_transport_state_ = status.state;
    NotifyTransportStatus(status);
    if (status.state == TransportState::Failed)
        ReportError("Terminal process failed to start");
    if (status.state == TransportState::Exited ||
        status.state == TransportState::Failed)
        ShowTransportNotice(status);
    RequestFrameRefresh();
}

void WxTerminalCtrl::OnTimer(wxTimerEvent&)
{
    impl_->AssertOwnerThread();
    if (impl_->selection_dragging_ && impl_->auto_scroll_direction_ != 0) {
        sakura_terminal_scroll_lines(impl_->core_,
                                     impl_->auto_scroll_direction_ < 0 ? 1 : -1);
        const auto [column, row] = CellAt(impl_->last_pointer_position_);
        sakura_terminal_update_selection(impl_->core_, column, row);
        RequestFrameRefresh();
    }
    const auto output = impl_->transport_ != nullptr
        ? impl_->transport_->TakeOutput() : std::vector<std::string> {};
    for (const auto& chunk : output)
        sakura_terminal_feed_output(impl_->core_, chunk.data(), chunk.size());
    if (!output.empty())
        RequestFrameRefresh();
    NotifyTitleChanged();
    UpdateTransportStatus();

    const auto now = std::chrono::steady_clock::now();
    if (impl_->callbacks_.on_metrics && impl_->config_.metrics_interval_ms > 0 &&
        now - impl_->last_metrics_callback_ >= std::chrono::milliseconds(
            impl_->config_.metrics_interval_ms)) {
        SakuraTerminalMetrics core_metrics {};
        sakura_terminal_get_metrics(impl_->core_, &core_metrics);
        const TransportMetrics transport_metrics = impl_->transport_ != nullptr
            ? impl_->transport_->GetMetrics() : TransportMetrics {};
        impl_->callbacks_.on_metrics(core_metrics, transport_metrics);
        impl_->last_metrics_callback_ = now;
    }

    if (impl_->trace_metrics_) {
        if (now - impl_->last_metrics_log_ >= std::chrono::seconds(1)) {
            SakuraTerminalMetrics core_metrics {};
            sakura_terminal_get_metrics(impl_->core_, &core_metrics);
            const TransportMetrics transport_metrics = impl_->transport_ != nullptr
                ? impl_->transport_->GetMetrics() : TransportMetrics {};
            std::fprintf(stderr,
                         "[metrics] output=%lluB/%llu chunks input=%llu "
                         "writes=%lluB/%llu renders=%llu selection=%llu "
                         "latency-max=%lluus/%llu paste=%lluB "
                         "mouse=%llu/%llu modes=%llu "
                         "paint=%llu full/%llu partial cells=%llu "
                         "paint-us=%llu/%llu/%llu max=%llu refresh=%llu/%llu dirty "
                         "glyph-cache=%llu/%llu bypass=%llu evictions=%llu "
                         "bytes=%llu/%llu "
                         "transport-read=%lluB/%llu "
                         "queue-high-water=%llu resize=%llu\n",
                         static_cast<unsigned long long>(core_metrics.output_bytes),
                         static_cast<unsigned long long>(core_metrics.output_chunks),
                         static_cast<unsigned long long>(core_metrics.input_events),
                         static_cast<unsigned long long>(core_metrics.transport_write_bytes),
                         static_cast<unsigned long long>(core_metrics.transport_write_events),
                         static_cast<unsigned long long>(core_metrics.rendered_frames),
                         static_cast<unsigned long long>(core_metrics.selection_copies),
                         static_cast<unsigned long long>(core_metrics.max_render_latency_us),
                         static_cast<unsigned long long>(core_metrics.render_latency_samples),
                         static_cast<unsigned long long>(core_metrics.paste_bytes),
                         static_cast<unsigned long long>(core_metrics.mouse_events),
                         static_cast<unsigned long long>(core_metrics.mouse_events_forwarded),
                         static_cast<unsigned long long>(core_metrics.mouse_mode_changes),
                         static_cast<unsigned long long>(impl_->paint_metrics_.full_repaints),
                         static_cast<unsigned long long>(impl_->paint_metrics_.partial_repaints),
                         static_cast<unsigned long long>(impl_->paint_metrics_.painted_cells),
                         static_cast<unsigned long long>(impl_->paint_metrics_.p50_paint_time_us),
                         static_cast<unsigned long long>(impl_->paint_metrics_.p95_paint_time_us),
                         static_cast<unsigned long long>(impl_->paint_metrics_.p99_paint_time_us),
                         static_cast<unsigned long long>(impl_->paint_metrics_.max_paint_time_us),
                         static_cast<unsigned long long>(impl_->paint_metrics_.refresh_requests),
                         static_cast<unsigned long long>(impl_->paint_metrics_.dirty_refresh_requests),
                         static_cast<unsigned long long>(impl_->paint_metrics_.glyph_run_cache_hits),
                         static_cast<unsigned long long>(impl_->paint_metrics_.glyph_run_cache_misses),
                         static_cast<unsigned long long>(impl_->paint_metrics_.glyph_run_cache_bypasses),
                         static_cast<unsigned long long>(impl_->paint_metrics_.glyph_run_cache_evictions),
                         static_cast<unsigned long long>(impl_->paint_metrics_.glyph_run_cache_bytes),
                         static_cast<unsigned long long>(impl_->paint_metrics_.glyph_run_cache_peak_bytes),
                         static_cast<unsigned long long>(transport_metrics.bytes_read),
                         static_cast<unsigned long long>(transport_metrics.read_events),
                         static_cast<unsigned long long>(transport_metrics.max_queued_bytes),
                         static_cast<unsigned long long>(transport_metrics.resize_events));
            impl_->last_metrics_log_ = now;
        }
    }
}

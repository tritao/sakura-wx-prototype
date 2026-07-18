#include "terminal_renderer.h"

#include <sakura/terminal/core_c.h>

#include <wx/dcclient.h>
#include <wx/dcmemory.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <iterator>
#include <list>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

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

wxBitmap CloneBitmap(wxBitmap& source)
{
    if (!source.IsOk())
        return {};
    wxBitmap copy(source.GetWidth(), source.GetHeight(), -1);
    if (!copy.IsOk())
        return {};

    wxMemoryDC source_dc;
    source_dc.SelectObject(source);
    wxMemoryDC copy_dc(copy);
    copy_dc.Blit(0, 0, source.GetWidth(), source.GetHeight(), &source_dc,
                 0, 0, wxCOPY);
    copy_dc.SelectObject(wxNullBitmap);
    source_dc.SelectObject(wxNullBitmap);
    return copy;
}

} // namespace

class WxRenderer::Impl {
public:
    struct ColorResources {
        wxColour color;
        wxBrush brush;
        wxPen pen;
    };

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

    using GlyphRunLru = std::list<GlyphRunKey>;
    using GlyphRunCache = std::unordered_map<
        GlyphRunKey, GlyphRunCacheEntry, GlyphRunKeyHash>;

    static constexpr std::size_t kPaintSampleWindow = 256;

    Impl(wxWindow& owner, const TerminalConfig& config, WxPaintMetrics& metrics)
        : owner_(owner),
          config_(config),
          metrics_(metrics),
          font_(CreateTerminalFont(config)),
          background_brush_(wxColour(config.background[0], config.background[1],
                                     config.background[2]))
    {
        trace_scroll_ = std::getenv("SAKURA_TRACE_SCROLL") != nullptr;
        glyph_font_identity_ = std::hash<std::string>{}(
            font_.GetNativeFontInfoDesc().ToStdString()) ^
            (std::hash<bool>{}(config.use_system_font) << 1) ^
            (static_cast<std::size_t>(std::max(1, config.font_size)) << 2);
    }

    bool UpdateGeometry()
    {
        wxClientDC dc(&owner_);
        dc.SetFont(font_);
        const int previous_cell_width = cell_width_;
        const int previous_cell_height = cell_height_;
        dc.GetTextExtent("M", &cell_width_, &cell_height_);
        cell_width_ = std::max(cell_width_, 1);
        cell_height_ = std::max(cell_height_, 1);
        if (previous_cell_width != cell_width_ ||
            previous_cell_height != cell_height_) {
            ClearGlyphRunCache();
            glyph_fonts_valid_.fill(false);
        }

        const wxSize size = owner_.GetClientSize();
        const unsigned int columns = static_cast<unsigned int>(
            std::max(1, size.GetWidth() / cell_width_));
        const unsigned int rows = static_cast<unsigned int>(
            std::max(1, size.GetHeight() / cell_height_));
        if (columns == columns_ && rows == rows_)
            return false;
        columns_ = columns;
        rows_ = rows;
        return true;
    }

    const wxFont& Font() const { return font_; }

    int FontSize() const { return font_.GetPointSize(); }
    wxSize CellSize() const { return {cell_width_, cell_height_}; }
    int CellWidth() const { return cell_width_; }
    int CellHeight() const { return cell_height_; }
    unsigned int Columns() const { return columns_; }
    unsigned int Rows() const { return rows_; }
    bool ScrollAnimationActive() const { return scroll_animation_active_; }
    void SetTraceScroll(bool enabled) { trace_scroll_ = enabled; }

    std::string FontFamily() const
    {
        const std::string family = ToUtf8(font_.GetFaceName());
        if (!family.empty())
            return family;
        return config_.use_system_font ? "system monospace" :
                                         config_.font_family;
    }

    void RecordPaintDuration(uint64_t elapsed_us)
    {
        metrics_.paint_time_us += elapsed_us;
        metrics_.max_paint_time_us = std::max(
            metrics_.max_paint_time_us, elapsed_us);
        paint_time_samples_[paint_time_sample_cursor_] = elapsed_us;
        paint_time_sample_cursor_ =
            (paint_time_sample_cursor_ + 1) % kPaintSampleWindow;
        paint_time_sample_count_ = std::min(
            paint_time_sample_count_ + 1, kPaintSampleWindow);

        auto sorted = paint_time_samples_;
        std::sort(sorted.begin(), sorted.begin() +
                  static_cast<std::ptrdiff_t>(paint_time_sample_count_));
        metrics_.p50_paint_time_us = PaintPercentile(
            sorted, paint_time_sample_count_, 50, 100);
        metrics_.p95_paint_time_us = PaintPercentile(
            sorted, paint_time_sample_count_, 95, 100);
        metrics_.p99_paint_time_us = PaintPercentile(
            sorted, paint_time_sample_count_, 99, 100);
    }

    void PaintFrame(wxDC& dc, const SakuraTerminalFrame* frame,
                    const SakuraTerminalFrameInfo& info,
                    const SakuraTerminalDirtyRegion& dirty,
                    bool full_repaint, uint64_t* painted_cells)
    {
        EnsureFramebuffer();
        TraceScroll(
            "frame changed=%d generation=%llu delta=%d kind=%d full=%d framebuffer=%d "
            "dirty=%u,%u-%u,%u",
            info.changed, static_cast<unsigned long long>(info.generation),
            info.scroll_delta, static_cast<int>(info.scroll_kind),
            full_repaint ? 1 : 0, framebuffer_valid_ ? 1 : 0,
            info.dirty.left, info.dirty.top, info.dirty.right, info.dirty.bottom);
        if (info.scroll_kind == SAKURA_TERMINAL_SCROLL_VIEWPORT &&
            info.scroll_delta != 0)
            BeginScrollAnimation(info);
        else
            CancelScrollAnimation(true, "paint-other-frame");

        wxMemoryDC framebuffer_dc;
        framebuffer_dc.SelectObject(framebuffer_);
        if (!framebuffer_valid_ || full_repaint) {
            ++metrics_.full_repaints;
            framebuffer_dc.SetBackground(wxBrush(wxColour(
                config_.background[0], config_.background[1],
                config_.background[2])));
            framebuffer_dc.Clear();
            const SakuraTerminalDirtyRegion full_region {
                0, 0, info.columns, info.rows
            };
            RenderFrame(framebuffer_dc, frame, info, full_region,
                        painted_cells);
            framebuffer_valid_ = true;
        } else if (info.changed) {
            if (info.scroll_delta != 0)
                ScrollFramebuffer(framebuffer_dc, info.scroll_delta,
                                  info.columns, info.rows, cell_width_,
                                  cell_height_);
            ++metrics_.partial_repaints;
            RenderFrame(framebuffer_dc, frame, info, dirty, painted_cells);
        }
        framebuffer_dc.SelectObject(wxNullBitmap);
        metrics_.painted_cells += painted_cells == nullptr ? 0 : *painted_cells;
        DrawScrollAnimation(dc);
    }

    void Draw(wxDC& dc)
    {
        if (scroll_animation_active_)
            DrawScrollAnimation(dc);
        else if (framebuffer_valid_)
            dc.DrawBitmap(framebuffer_, 0, 0, false);
    }

    bool AdvanceScrollAnimation()
    {
        if (!scroll_animation_active_)
            return false;

        const auto elapsed = std::chrono::steady_clock::now() -
            scroll_animation_started_;
        const double duration = std::max<double>(
            1.0, scroll_animation_duration_.count());
        const double normalized = std::min<double>(1.0,
            std::chrono::duration<double, std::milli>(elapsed).count() /
                duration);
        const int next_progress = static_cast<int>(std::lround(
            normalized * scroll_animation_distance_px_));
        const bool changed = next_progress != scroll_animation_progress_px_;
        if (changed) {
            scroll_animation_progress_px_ = next_progress;
            ++metrics_.scroll_animation_frames;
        }
        TraceScroll(
            "animation advance elapsed_us=%lld progress_px=%d/%d changed=%d active=%d",
            static_cast<long long>(std::chrono::duration_cast<
                std::chrono::microseconds>(elapsed).count()),
            next_progress, scroll_animation_distance_px_, changed ? 1 : 0,
            normalized < 1.0 ? 1 : 0);
        if (normalized >= 1.0) {
            scroll_animation_active_ = false;
            scroll_animation_source_ = wxNullBitmap;
            scroll_animation_direction_ = 0;
            ++metrics_.scroll_animation_completions;
            return true;
        }
        return changed;
    }

    void CancelScrollAnimation(bool forced, const char* reason)
    {
        if (!scroll_animation_active_)
            return;
        TraceScroll("animation cancel forced=%d reason=%s", forced ? 1 : 0,
                    reason == nullptr ? "unknown" : reason);
        scroll_animation_active_ = false;
        scroll_animation_source_ = wxNullBitmap;
        scroll_animation_progress_px_ = scroll_animation_distance_px_;
        scroll_animation_direction_ = 0;
        if (forced)
            ++metrics_.scroll_animation_settles;
    }

private:
    static uint64_t PaintPercentile(
        const std::array<uint64_t, kPaintSampleWindow>& sorted,
        std::size_t count, std::size_t numerator, std::size_t denominator)
    {
        const std::size_t rank = std::max<std::size_t>(
            1, (count * numerator + denominator - 1) / denominator);
        return sorted[rank - 1];
    }

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

    void TraceScroll(const char* format, ...) const
    {
        if (!trace_scroll_)
            return;
        const auto elapsed = std::chrono::duration_cast<
            std::chrono::microseconds>(std::chrono::steady_clock::now() -
                                       trace_started_).count();
        std::fprintf(stderr, "[scroll +%lluus] ",
                     static_cast<unsigned long long>(std::max<int64_t>(
                         0, elapsed)));
        va_list arguments;
        va_start(arguments, format);
        std::vfprintf(stderr, format, arguments);
        va_end(arguments);
        std::fputc('\n', stderr);
    }

    void UpdateGlyphCacheMetrics()
    {
        metrics_.glyph_run_cache_entries =
            static_cast<uint64_t>(glyph_runs_.size());
        metrics_.glyph_run_cache_bytes =
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
        ++metrics_.glyph_run_cache_evictions;
        UpdateGlyphCacheMetrics();
    }

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

    const wxFont& GlyphFont(uint8_t attributes)
    {
        const std::size_t index = attributes & 0x07;
        if (!glyph_fonts_valid_[index]) {
            wxFont& glyph_font = glyph_fonts_[index];
            glyph_font = font_;
            glyph_font.SetWeight((attributes & 0x01) != 0
                ? wxFONTWEIGHT_BOLD : wxFONTWEIGHT_NORMAL);
            glyph_font.SetStyle((attributes & 0x02) != 0
                ? wxFONTSTYLE_ITALIC : wxFONTSTYLE_NORMAL);
            glyph_font.SetUnderlined((attributes & 0x04) != 0);
            glyph_fonts_valid_[index] = true;
        }
        return glyph_fonts_[index];
    }

    const wxString& GlyphText(const char* text, std::size_t length)
    {
        constexpr std::size_t max_glyph_texts = 4096;
        const std::string key(text == nullptr ? "" : std::string(text, length));
        const auto existing = glyph_texts_.find(key);
        if (existing != glyph_texts_.end())
            return existing->second;
        if (glyph_texts_.size() >= max_glyph_texts)
            glyph_texts_.clear();
        const auto inserted = glyph_texts_.emplace(
            key, wxString::FromUTF8(key.c_str()));
        return inserted.first->second;
    }

    const wxBitmap& GlyphRunBitmap(
        wxDC& dc, const SakuraTerminalRunView& run, const wxString& text,
        const std::array<uint8_t, 3>& foreground,
        const std::array<uint8_t, 3>& background)
    {
        GlyphRunKey key;
        key.text.assign(run.text == nullptr ? "" : run.text, run.text_length);
        key.foreground = ColorKey(foreground);
        key.background = ColorKey(background);
        key.font_identity = glyph_font_identity_;
        key.cell_count = run.cell_count;
        key.attributes = run.attributes;
        key.cell_width = cell_width_;
        key.cell_height = cell_height_;
        const wxSize ppi = dc.GetPPI();
        key.dpi_x = ppi.GetWidth();
        key.dpi_y = ppi.GetHeight();

        const auto existing = glyph_runs_.find(key);
        if (existing != glyph_runs_.end()) {
            glyph_run_lru_.splice(glyph_run_lru_.begin(), glyph_run_lru_,
                                  existing->second.lru);
            ++metrics_.glyph_run_cache_hits;
            return existing->second.resource.bitmap;
        }

        ++metrics_.glyph_run_cache_misses;
        const wxFont& font = GlyphFont(run.attributes);
        dc.SetFont(font);
        ++metrics_.dc_state_changes;
        const wxSize measured_size = dc.GetTextExtent(text);
        const int run_width = static_cast<int>(run.cell_count) * cell_width_;
        const int bitmap_width = std::max(1, std::max(
            run_width, measured_size.GetWidth()));
        const int bitmap_height = std::max(1, cell_height_);

        wxBitmap bitmap(bitmap_width, bitmap_height, -1);
        if (bitmap.IsOk()) {
            wxMemoryDC glyph_dc(bitmap);
            glyph_dc.SetBackground(wxBrush(wxColour(
                background[0], background[1], background[2])));
            glyph_dc.Clear();
            glyph_dc.SetTextForeground(wxColour(
                foreground[0], foreground[1], foreground[2]));
            glyph_dc.SetFont(font);
            glyph_dc.DrawText(text, 0, 0);
            glyph_dc.SelectObject(wxNullBitmap);
        }

        const std::size_t bitmap_bytes = BitmapBytes(bitmap);
        const bool cacheable = bitmap.IsOk() &&
            config_.glyph_cache_max_entries > 0 &&
            bitmap_bytes <= config_.glyph_cache_max_bytes;
        if (!cacheable) {
            glyph_bitmap_scratch_ = bitmap;
            return glyph_bitmap_scratch_;
        }

        while (!glyph_run_lru_.empty() &&
               (glyph_runs_.size() >= config_.glyph_cache_max_entries ||
                glyph_run_cache_bytes_ >
                    config_.glyph_cache_max_bytes - bitmap_bytes))
            EvictLeastRecentlyUsedGlyphRun();

        auto inserted = glyph_runs_.emplace(
            std::move(key), GlyphRunCacheEntry {});
        inserted.first->second.resource.bitmap = bitmap;
        inserted.first->second.resource.measured_size = measured_size;
        inserted.first->second.bytes = bitmap_bytes;
        glyph_run_lru_.push_front(inserted.first->first);
        inserted.first->second.lru = glyph_run_lru_.begin();
        glyph_run_cache_bytes_ += bitmap_bytes;
        metrics_.glyph_run_cache_peak_bytes = std::max(
            metrics_.glyph_run_cache_peak_bytes,
            static_cast<uint64_t>(glyph_run_cache_bytes_));
        UpdateGlyphCacheMetrics();
        return inserted.first->second.resource.bitmap;
    }

    void RenderFrame(wxDC& dc, const SakuraTerminalFrame* frame,
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
        ++metrics_.dc_state_changes;
        dc.SetBrush(background_brush_);
        ++metrics_.dc_state_changes;
        dc.DrawRectangle(left * cell_width_, top * cell_height_,
                         (right - left) * cell_width_,
                         (bottom - top) * cell_height_);
        ++metrics_.background_rectangles;

        const uint32_t default_background = ColorKey(config_.background);
        const bool cache_glyphs = config_.glyph_cache_enabled &&
            (!config_.glyph_cache_bypass_scroll || info.scroll_delta == 0);
        const unsigned int glyph_run_span_cells = std::max(
            2u, config_.glyph_cache_max_run_cells);

        for (unsigned int row = top; row < bottom; ++row) {
            const std::size_t bounded_run_count = cache_glyphs
                ? sakura_terminal_frame_row_span_count(
                      frame, row, glyph_run_span_cells) : 0;
            const bool use_bounded_spans = cache_glyphs && bounded_run_count > 0;
            const std::size_t run_count = use_bounded_spans
                ? bounded_run_count
                : sakura_terminal_frame_row_run_count(frame, row);
            std::vector<SakuraTerminalRunView> row_runs;
            row_runs.reserve(run_count);
            for (std::size_t index = 0; index < run_count; ++index) {
                SakuraTerminalRunView run {};
                const int valid = use_bounded_spans
                    ? sakura_terminal_frame_row_span(
                          frame, row, index, glyph_run_span_cells, &run)
                    : sakura_terminal_frame_row_run(frame, row, index, &run);
                if (valid)
                    row_runs.push_back(run);
            }
            if (use_bounded_spans && row_runs.empty()) {
                const std::size_t logical_run_count =
                    sakura_terminal_frame_row_run_count(frame, row);
                row_runs.reserve(logical_run_count);
                for (std::size_t index = 0; index < logical_run_count; ++index) {
                    SakuraTerminalRunView run {};
                    if (sakura_terminal_frame_row_run(frame, row, index, &run))
                        row_runs.push_back(run);
                }
            }

            bool pending_background = false;
            uint32_t pending_background_key = 0;
            std::array<uint8_t, 3> pending_background_color {};
            unsigned int pending_background_left = 0;
            unsigned int pending_background_right = 0;
            const auto flush_background = [&]() {
                if (!pending_background)
                    return;
                dc.SetBrush(ColorResourcesFor(pending_background_color).brush);
                ++metrics_.dc_state_changes;
                dc.DrawRectangle(
                    pending_background_left * cell_width_,
                    row * cell_height_,
                    (pending_background_right - pending_background_left) *
                        cell_width_,
                    cell_height_);
                ++metrics_.background_rectangles;
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

                const auto draw_glyph_run = [&](
                    const SakuraTerminalRunView& glyph_run,
                    const wxString& glyph) {
                    if (cache_glyphs) {
                        const wxBitmap& glyph_bitmap = GlyphRunBitmap(
                            dc, glyph_run, glyph, foreground, background);
                        if (glyph_bitmap.IsOk()) {
                            dc.DrawBitmap(glyph_bitmap,
                                          glyph_run.left * cell_width_,
                                          row * cell_height_, false);
                            ++metrics_.glyph_bitmap_draws;
                        } else {
                            dc.SetTextForeground(
                                ColorResourcesFor(foreground).color);
                            ++metrics_.dc_state_changes;
                            dc.SetFont(GlyphFont(glyph_run.attributes));
                            ++metrics_.dc_state_changes;
                            dc.DrawText(glyph, glyph_run.left * cell_width_,
                                        row * cell_height_);
                            ++metrics_.glyph_text_draws;
                        }
                    } else {
                        if (config_.glyph_cache_enabled &&
                            config_.glyph_cache_bypass_scroll &&
                            info.scroll_delta != 0)
                            ++metrics_.glyph_run_cache_bypasses;
                        dc.SetTextForeground(ColorResourcesFor(foreground).color);
                        ++metrics_.dc_state_changes;
                        dc.SetFont(GlyphFont(glyph_run.attributes));
                        ++metrics_.dc_state_changes;
                        dc.DrawText(glyph, glyph_run.left * cell_width_,
                                    row * cell_height_);
                        ++metrics_.glyph_text_draws;
                    }
                };

                const auto draw_text_run =
                    [&](const SakuraTerminalRunView& text_run) {
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
                    if (!has_glyph)
                        return;
                    const wxString& glyph = GlyphText(
                        text_run.text, text_run.text_length);
                    if (!glyph.empty()) {
                        ++metrics_.glyph_run_spans;
                        draw_glyph_run(text_run, glyph);
                    }
                };
                draw_text_run(run);
                if ((run.attributes & 0x04) != 0) {
                    dc.SetPen(ColorResourcesFor(foreground).pen);
                    ++metrics_.dc_state_changes;
                    const int underline_y = (row + 1) * cell_height_ - 2;
                    dc.DrawLine(draw_left * cell_width_, underline_y,
                                draw_right * cell_width_ - 1, underline_y);
                    dc.SetPen(*wxTRANSPARENT_PEN);
                    ++metrics_.dc_state_changes;
                }
            }
        }

        const bool cursor_in_dirty_region =
            info.cursor_x >= left && info.cursor_x < right &&
            info.cursor_y >= top && info.cursor_y < bottom;
        if (cursor_in_dirty_region && info.cursor_visible &&
            info.cursor_x < info.columns && info.cursor_y < info.rows) {
            dc.SetBrush(*wxTRANSPARENT_BRUSH);
            ++metrics_.dc_state_changes;
            dc.SetPen(wxPen(wxColour(230, 230, 230), 1));
            ++metrics_.dc_state_changes;
            const int cursor_x = info.cursor_x * cell_width_;
            const int cursor_y = info.cursor_y * cell_height_;
            switch (info.cursor_style) {
            case SAKURA_TERMINAL_CURSOR_UNDERLINE:
                dc.DrawLine(cursor_x, cursor_y + cell_height_ - 2,
                            cursor_x + cell_width_ - 1,
                            cursor_y + cell_height_ - 2);
                break;
            case SAKURA_TERMINAL_CURSOR_BAR:
                dc.DrawLine(cursor_x + 1, cursor_y + 1,
                            cursor_x + 1, cursor_y + cell_height_ - 2);
                break;
            case SAKURA_TERMINAL_CURSOR_BLOCK:
                dc.DrawRectangle(cursor_x, cursor_y, cell_width_, cell_height_);
                break;
            }
        }
    }

    void EnsureFramebuffer()
    {
        const wxSize client_size = owner_.GetClientSize();
        const int bitmap_width = std::max(1, client_size.GetWidth());
        const int bitmap_height = std::max(1, client_size.GetHeight());
        if (!framebuffer_.IsOk() || framebuffer_.GetWidth() != bitmap_width ||
            framebuffer_.GetHeight() != bitmap_height) {
            CancelScrollAnimation(true, "paint-resize");
            framebuffer_ = wxBitmap(bitmap_width, bitmap_height, -1);
            framebuffer_valid_ = false;
            ++metrics_.framebuffer_rebuilds;
        }
    }

    void BeginScrollAnimation(const SakuraTerminalFrameInfo& info)
    {
        if (!config_.smooth_scrolling || !framebuffer_valid_ ||
            info.scroll_delta == 0 ||
            info.scroll_kind != SAKURA_TERMINAL_SCROLL_VIEWPORT) {
            TraceScroll(
                "animation reject enabled=%d framebuffer=%d delta=%d kind=%d",
                config_.smooth_scrolling ? 1 : 0,
                framebuffer_valid_ ? 1 : 0, info.scroll_delta,
                static_cast<int>(info.scroll_kind));
            return;
        }

        if (scroll_animation_active_)
            CancelScrollAnimation(true, "animation-restart");
        scroll_animation_source_ = CloneBitmap(framebuffer_);
        if (!scroll_animation_source_.IsOk())
            return;

        const unsigned int lines = static_cast<unsigned int>(
            std::abs(info.scroll_delta));
        scroll_animation_distance_px_ = lines * cell_height_;
        if (scroll_animation_distance_px_ <= 0)
            return;

        const uint64_t requested_ms = static_cast<uint64_t>(std::max(
            1u, config_.scroll_animation_ms_per_line)) * lines;
        const uint64_t max_ms = config_.scroll_animation_max_ms == 0
            ? requested_ms : config_.scroll_animation_max_ms;
        scroll_animation_duration_ = std::chrono::milliseconds(
            static_cast<int64_t>(std::max<uint64_t>(1,
                std::min(requested_ms, max_ms))));
        scroll_animation_progress_px_ = 0;
        scroll_animation_direction_ = info.scroll_delta > 0 ? 1 : -1;
        scroll_animation_started_ = std::chrono::steady_clock::now();
        scroll_animation_active_ = true;
        ++metrics_.scroll_animation_starts;
        TraceScroll(
            "animation start delta=%d full=%d distance_px=%d duration_ms=%lld",
            info.scroll_delta, info.full_repaint ? 1 : 0,
            scroll_animation_distance_px_,
            static_cast<long long>(scroll_animation_duration_.count()));
    }

    void DrawScrollAnimation(wxDC& dc)
    {
        if (!scroll_animation_active_ || !scroll_animation_source_.IsOk()) {
            if (framebuffer_valid_)
                dc.DrawBitmap(framebuffer_, 0, 0, false);
            return;
        }

        ++metrics_.scroll_animation_paints;
        TraceScroll("animation paint progress_px=%d/%d direction=%d",
                    scroll_animation_progress_px_,
                    scroll_animation_distance_px_,
                    scroll_animation_direction_);
        const int width = framebuffer_.GetWidth();
        const int grid_height = std::min(
            framebuffer_.GetHeight(), static_cast<int>(rows_) * cell_height_);
        const int progress = std::clamp(
            scroll_animation_progress_px_, 0,
            std::min(scroll_animation_distance_px_, grid_height));
        const int distance = scroll_animation_distance_px_;
        dc.SetBackground(background_brush_);
        dc.Clear();
        if (scroll_animation_direction_ > 0) {
            dc.SetClippingRegion(0, 0, width, grid_height);
            dc.DrawBitmap(scroll_animation_source_, 0, -progress, false);
            dc.DestroyClippingRegion();
            if (progress > 0) {
                dc.SetClippingRegion(0, grid_height - progress, width, progress);
                dc.DrawBitmap(framebuffer_, 0, distance - progress, false);
                dc.DestroyClippingRegion();
            }
        } else {
            dc.SetClippingRegion(0, 0, width, grid_height);
            dc.DrawBitmap(scroll_animation_source_, 0, progress, false);
            dc.DestroyClippingRegion();
            if (progress > 0) {
                dc.SetClippingRegion(0, 0, width, progress);
                dc.DrawBitmap(framebuffer_, 0, -distance + progress, false);
                dc.DestroyClippingRegion();
            }
        }
    }

private:
    wxWindow& owner_;
    const TerminalConfig& config_;
    WxPaintMetrics& metrics_;
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
    wxBitmap scroll_animation_source_;
    bool scroll_animation_active_ = false;
    int scroll_animation_distance_px_ = 0;
    int scroll_animation_progress_px_ = 0;
    int scroll_animation_direction_ = 0;
    std::chrono::steady_clock::time_point scroll_animation_started_;
    std::chrono::milliseconds scroll_animation_duration_ {0};
    std::array<uint64_t, kPaintSampleWindow> paint_time_samples_ {};
    std::size_t paint_time_sample_count_ = 0;
    std::size_t paint_time_sample_cursor_ = 0;
    int cell_width_ = 8;
    int cell_height_ = 16;
    unsigned int columns_ = 80;
    unsigned int rows_ = 24;
    bool trace_scroll_ = false;
    std::chrono::steady_clock::time_point trace_started_ =
        std::chrono::steady_clock::now();
};

WxRenderer::WxRenderer(wxWindow& owner, const TerminalConfig& config,
                       WxPaintMetrics& metrics)
    : impl_(std::make_unique<Impl>(owner, config, metrics))
{
}

WxRenderer::~WxRenderer() = default;

bool WxRenderer::UpdateGeometry() { return impl_->UpdateGeometry(); }
const wxFont& WxRenderer::Font() const { return impl_->Font(); }
std::string WxRenderer::FontFamily() const { return impl_->FontFamily(); }
int WxRenderer::FontSize() const { return impl_->Font().GetPointSize(); }
wxSize WxRenderer::CellSize() const
{
    return impl_->CellSize();
}
int WxRenderer::CellWidth() const { return impl_->CellWidth(); }
int WxRenderer::CellHeight() const { return impl_->CellHeight(); }
unsigned int WxRenderer::Columns() const { return impl_->Columns(); }
unsigned int WxRenderer::Rows() const { return impl_->Rows(); }
void WxRenderer::RecordPaintDuration(uint64_t elapsed_us)
{
    impl_->RecordPaintDuration(elapsed_us);
}
void WxRenderer::PaintFrame(wxDC& dc, const SakuraTerminalFrame* frame,
                            const SakuraTerminalFrameInfo& info,
                            const SakuraTerminalDirtyRegion& dirty,
                            bool full_repaint, uint64_t* painted_cells)
{
    impl_->PaintFrame(dc, frame, info, dirty, full_repaint, painted_cells);
}
void WxRenderer::Draw(wxDC& dc) { impl_->Draw(dc); }
bool WxRenderer::AdvanceScrollAnimation()
{
    return impl_->AdvanceScrollAnimation();
}
void WxRenderer::CancelScrollAnimation(bool forced, const char* reason)
{
    impl_->CancelScrollAnimation(forced, reason);
}
bool WxRenderer::ScrollAnimationActive() const
{
    return impl_->ScrollAnimationActive();
}
void WxRenderer::SetTraceScroll(bool enabled)
{
    impl_->SetTraceScroll(enabled);
}

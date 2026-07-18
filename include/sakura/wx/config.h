#pragma once

#include <sakura/terminal/core_c.h>
#include <sakura/terminal/transport.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

struct TerminalConfig {
    std::string font_family = "DejaVu Sans Mono";
    int font_size = 10;
    // Use wxWidgets' platform monospace family by default. Set this to false
    // to honor font_family as an explicit face-name override.
    bool use_system_font = true;
    std::array<uint8_t, 3> background {16, 18, 20};
    std::array<uint8_t, 3> error_foreground {240, 180, 90};
    unsigned int timer_interval_ms = 16;
    unsigned int metrics_interval_ms = 1000;
    bool start_transport = true;
    bool glyph_cache_enabled = true;
    bool glyph_cache_bypass_scroll = true;
    std::size_t glyph_cache_max_bytes = 4u * 1024u * 1024u;
    std::size_t glyph_cache_max_entries = 1024;
};

struct WxPaintMetrics {
    uint64_t paint_events = 0;
    uint64_t full_repaints = 0;
    uint64_t partial_repaints = 0;
    uint64_t framebuffer_rebuilds = 0;
    uint64_t painted_cells = 0;
    uint64_t paint_time_us = 0;
    uint64_t max_paint_time_us = 0;
    uint64_t p50_paint_time_us = 0;
    uint64_t p95_paint_time_us = 0;
    uint64_t p99_paint_time_us = 0;
    uint64_t refresh_requests = 0;
    uint64_t full_refresh_requests = 0;
    uint64_t dirty_refresh_requests = 0;
    uint64_t wheel_events = 0;
    uint64_t wheel_partial_events = 0;
    uint64_t wheel_scroll_updates = 0;
    uint64_t wheel_lines_scrolled = 0;
    uint64_t glyph_run_cache_hits = 0;
    uint64_t glyph_run_cache_misses = 0;
    uint64_t glyph_run_cache_bypasses = 0;
    uint64_t glyph_run_spans = 0;
    uint64_t glyph_run_cache_evictions = 0;
    uint64_t glyph_run_cache_entries = 0;
    uint64_t glyph_run_cache_bytes = 0;
    uint64_t glyph_run_cache_peak_bytes = 0;
    uint64_t background_rectangles = 0;
    uint64_t glyph_bitmap_draws = 0;
    uint64_t glyph_text_draws = 0;
    uint64_t dc_state_changes = 0;
};

struct TerminalCallbacks {
    // Callbacks execute synchronously on the WxTerminalCtrl/UI thread. They
    // must not destroy or re-enter the control; defer such work to the host
    // event loop instead.
    std::function<void(const std::string&)> on_title_changed;
    std::function<void(const TransportStatus&)> on_transport_status_changed;
    std::function<void(const std::string&)> on_error;
    std::function<void(const SakuraTerminalMetrics&, const TransportMetrics&)> on_metrics;
};

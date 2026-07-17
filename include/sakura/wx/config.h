#pragma once

#include <sakura/terminal/core.h>
#include <sakura/terminal/transport.h>

#include <array>
#include <cstdint>
#include <functional>
#include <string>

struct TerminalConfig {
    std::string font_family = "DejaVu Sans Mono";
    int font_size = 12;
    std::array<uint8_t, 3> background {16, 18, 20};
    std::array<uint8_t, 3> error_foreground {240, 180, 90};
    unsigned int timer_interval_ms = 16;
    unsigned int metrics_interval_ms = 1000;
    bool start_transport = true;
};

struct WxPaintMetrics {
    uint64_t paint_events = 0;
    uint64_t full_repaints = 0;
    uint64_t partial_repaints = 0;
    uint64_t framebuffer_rebuilds = 0;
    uint64_t painted_cells = 0;
    uint64_t paint_time_us = 0;
    uint64_t max_paint_time_us = 0;
    uint64_t refresh_requests = 0;
    uint64_t full_refresh_requests = 0;
    uint64_t dirty_refresh_requests = 0;
};

struct TerminalCallbacks {
    // Callbacks execute synchronously on the WxTerminalCtrl/UI thread. They
    // must not destroy or re-enter the control; defer such work to the host
    // event loop instead.
    std::function<void(const std::string&)> on_title_changed;
    std::function<void(const TransportStatus&)> on_transport_status_changed;
    std::function<void(const std::string&)> on_error;
    std::function<void(const TerminalMetrics&, const TransportMetrics&)> on_metrics;
};

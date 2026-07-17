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

struct TerminalCallbacks {
    std::function<void(const std::string&)> on_title_changed;
    std::function<void(const TransportStatus&)> on_transport_status_changed;
    std::function<void(const std::string&)> on_error;
    std::function<void(const TerminalMetrics&, const TransportMetrics&)> on_metrics;
};

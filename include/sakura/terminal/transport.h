#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

struct TransportMetrics {
    uint64_t bytes_read = 0;
    uint64_t read_events = 0;
    uint64_t bytes_written = 0;
    uint64_t write_events = 0;
    uint64_t resize_events = 0;
    uint64_t max_queued_bytes = 0;
};

enum class TransportState {
    Stopped,
    Starting,
    Running,
    Exited,
    Failed,
};

struct TransportStatus {
    TransportState state = TransportState::Stopped;
    int exit_code = -1;
    int signal = 0;
    bool exit_code_valid = false;
};

class TerminalTransport {
public:
    virtual ~TerminalTransport() = default;

    // Start, Stop, Write, Resize, and TakeOutput are owned by one application
    // thread. Implementations may use worker threads internally, but callers
    // must not invoke these lifecycle/I/O methods concurrently.
    virtual bool Start(unsigned int columns, unsigned int rows,
                       const std::string& shell = {}) = 0;
    virtual void Stop() = 0;
    virtual bool Write(const char* data, std::size_t length) = 0;
    virtual bool Resize(unsigned int columns, unsigned int rows) = 0;
    // Return at most max_bytes of queued process output. Limiting one drain
    // keeps UI event loops responsive when a child writes continuously (for
    // example, `yes`). A partial chunk remains queued for the next drain.
    virtual std::vector<std::string> TakeOutput(
        std::size_t max_bytes = std::numeric_limits<std::size_t>::max()) = 0;
    // Drop output already queued by an interrupted/flooding command. This is
    // owner-thread-only and does not affect output received afterwards.
    virtual void DiscardOutput() = 0;

    // Status and metrics are snapshots and may be read while the transport's
    // reader thread is active.
    virtual bool IsRunning() const = 0;
    virtual TransportStatus GetStatus() const = 0;
    virtual TransportMetrics GetMetrics() const = 0;
};

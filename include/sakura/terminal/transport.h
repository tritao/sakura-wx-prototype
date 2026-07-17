#pragma once

#include <cstddef>
#include <cstdint>
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

    virtual bool Start(unsigned int columns, unsigned int rows,
                       const std::string& shell = {}) = 0;
    virtual void Stop() = 0;
    virtual bool Write(const char* data, std::size_t length) = 0;
    virtual bool Resize(unsigned int columns, unsigned int rows) = 0;
    virtual std::vector<std::string> TakeOutput() = 0;
    virtual bool IsRunning() const = 0;
    virtual TransportStatus GetStatus() const = 0;
    virtual TransportMetrics GetMetrics() const = 0;
};

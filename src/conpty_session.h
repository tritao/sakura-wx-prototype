#pragma once

#include <sakura/terminal/transport.h>

#include <atomic>
#include <cstdint>
#include <deque>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class ConPtySession final : public TerminalTransport {
public:
    ConPtySession() = default;
    ~ConPtySession() override;

    ConPtySession(const ConPtySession&) = delete;
    ConPtySession& operator=(const ConPtySession&) = delete;

    bool Start(unsigned int columns, unsigned int rows,
               const std::string& shell = {}) override;
    void Stop() override;
    bool Write(const char* data, std::size_t length) override;
    bool Resize(unsigned int columns, unsigned int rows) override;
    std::vector<std::string> TakeOutput(
        std::size_t max_bytes = std::numeric_limits<std::size_t>::max()) override;
    void DiscardOutput() override;
    bool IsRunning() const override { return running_.load(); }
    TransportStatus GetStatus() const override;
    TransportMetrics GetMetrics() const override;

private:
    void ReadLoop(void* output_handle);

    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> running_{false};
    std::atomic<TransportState> state_{TransportState::Stopped};
    std::atomic<int> exit_code_{-1};
    std::atomic<int> exit_signal_{0};
    std::atomic<bool> exit_code_valid_{false};
    std::atomic<uint64_t> bytes_read_{0};
    std::atomic<uint64_t> read_events_{0};
    std::atomic<uint64_t> bytes_written_{0};
    std::atomic<uint64_t> write_events_{0};
    std::atomic<uint64_t> resize_events_{0};
    std::atomic<uint64_t> queued_bytes_{0};
    std::atomic<uint64_t> max_queued_bytes_{0};
    std::mutex io_mutex_;
    std::mutex output_mutex_;
    std::deque<std::string> output_;
    std::thread reader_thread_;
    void* input_write_ = nullptr;
    void* output_read_ = nullptr;
    void* pseudo_console_ = nullptr;
    void* process_handle_ = nullptr;
    void* process_job_ = nullptr;
};

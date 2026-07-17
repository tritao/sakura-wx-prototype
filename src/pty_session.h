#pragma once

#include <atomic>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <sakura/terminal/transport.h>

class PosixPtySession final : public TerminalTransport {
public:
    PosixPtySession() = default;
    ~PosixPtySession() override;

    PosixPtySession(const PosixPtySession&) = delete;
    PosixPtySession& operator=(const PosixPtySession&) = delete;

    bool Start(unsigned int columns, unsigned int rows,
               const std::string& shell = {}) override;
    void Stop() override;

    bool Write(const char* data, std::size_t length) override;
    bool Resize(unsigned int columns, unsigned int rows) override;

    std::vector<std::string> TakeOutput() override;
    bool IsRunning() const override { return running_.load(); }
    TransportStatus GetStatus() const override;
    TransportMetrics GetMetrics() const override;

private:
    void ReadLoop(int fd);

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
    int master_fd_ = -1;
    int child_pid_ = -1;
};

#pragma once

#include <atomic>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "terminal_transport.h"

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

private:
    void ReadLoop(int fd);

    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> running_{false};
    std::mutex io_mutex_;
    std::mutex output_mutex_;
    std::deque<std::string> output_;
    std::thread reader_thread_;
    int master_fd_ = -1;
    int child_pid_ = -1;
};

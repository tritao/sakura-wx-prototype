#pragma once

#include <atomic>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class PtySession final {
public:
    PtySession() = default;
    ~PtySession();

    PtySession(const PtySession&) = delete;
    PtySession& operator=(const PtySession&) = delete;

    bool Start(unsigned int columns, unsigned int rows, const std::string& shell = {});
    void Stop();

    bool Write(const char* data, std::size_t length);
    bool Resize(unsigned int columns, unsigned int rows);

    std::vector<std::string> TakeOutput();
    bool IsRunning() const { return running_.load(); }

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

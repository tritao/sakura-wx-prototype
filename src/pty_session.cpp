#include "pty_session.h"

#if defined(SAKURA_WX_POSIX)

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <util.h>
#else
#include <pty.h>
#endif

namespace {

void RecordMaximum(std::atomic<uint64_t>& maximum, uint64_t value)
{
    uint64_t current = maximum.load();
    while (current < value &&
           !maximum.compare_exchange_weak(current, value)) {
    }
}

void RecordExitStatus(std::atomic<int>& exit_code,
                      std::atomic<int>& exit_signal,
                      std::atomic<bool>& exit_code_valid,
                      int status)
{
    if (WIFEXITED(status)) {
        exit_code.store(WEXITSTATUS(status));
        exit_signal.store(0);
        exit_code_valid.store(true);
    } else if (WIFSIGNALED(status)) {
        exit_code.store(-1);
        exit_signal.store(WTERMSIG(status));
        exit_code_valid.store(true);
    }
}

} // namespace

#endif

PosixPtySession::~PosixPtySession()
{
    Stop();
}

bool PosixPtySession::Start(unsigned int columns, unsigned int rows,
                            const std::string& shell)
{
    Stop();
    state_.store(TransportState::Starting);
    exit_code_.store(-1);
    exit_signal_.store(0);
    exit_code_valid_.store(false);

#if defined(SAKURA_WX_POSIX)
    struct winsize size {};
    size.ws_col = static_cast<unsigned short>(columns);
    size.ws_row = static_cast<unsigned short>(rows);

    int master = -1;
    const pid_t child = forkpty(&master, nullptr, nullptr, &size);
    if (child < 0) {
        state_.store(TransportState::Failed);
        return false;
    }

    if (child == 0) {
        ::setenv("TERM", "xterm-256color", 1);
        ::setenv("COLORTERM", "truecolor", 1);

        const char* selected_shell = shell.empty() ? std::getenv("SHELL") : shell.c_str();
        if (selected_shell == nullptr || *selected_shell == '\0')
            selected_shell = "/bin/sh";

        ::execl(selected_shell, selected_shell, "-i", static_cast<char*>(nullptr));
        ::_exit(127);
    }

    {
        std::lock_guard lock(io_mutex_);
        master_fd_ = master;
        child_pid_ = static_cast<int>(child);
    }

    stop_requested_.store(false);
    running_.store(true);
    state_.store(TransportState::Running);
    reader_thread_ = std::thread(&PosixPtySession::ReadLoop, this, master);
    return true;
#else
    (void)columns;
    (void)rows;
    (void)shell;
    state_.store(TransportState::Failed);
    return false;
#endif
}

void PosixPtySession::Stop()
{
#if defined(SAKURA_WX_POSIX)
    stop_requested_.store(true);

    int child = -1;
    {
        std::lock_guard lock(io_mutex_);
        child = child_pid_;
    }
    if (child > 0)
        ::kill(static_cast<pid_t>(child), SIGHUP);

    if (reader_thread_.joinable())
        reader_thread_.join();

    int master = -1;
    {
        std::lock_guard lock(io_mutex_);
        master = master_fd_;
        master_fd_ = -1;
        child_pid_ = -1;
    }
    if (master >= 0)
        ::close(master);

    if (child > 0) {
        int status = 0;
        const pid_t result = ::waitpid(static_cast<pid_t>(child), &status, WNOHANG);
        if (result == 0) {
            ::kill(static_cast<pid_t>(child), SIGTERM);
            const pid_t waited = ::waitpid(static_cast<pid_t>(child), &status, 0);
            if (waited == static_cast<pid_t>(child))
                RecordExitStatus(exit_code_, exit_signal_, exit_code_valid_, status);
        } else if (result == static_cast<pid_t>(child)) {
            RecordExitStatus(exit_code_, exit_signal_, exit_code_valid_, status);
        }
    }
#else
    if (reader_thread_.joinable())
        reader_thread_.join();
#endif

    running_.store(false);
    const TransportState state = state_.load();
    if (state == TransportState::Starting || state == TransportState::Running)
        state_.store(TransportState::Stopped);
}

bool PosixPtySession::Write(const char* data, std::size_t length)
{
#if defined(SAKURA_WX_POSIX)
    std::lock_guard lock(io_mutex_);
    if (master_fd_ < 0)
        return false;

    std::size_t offset = 0;
    while (offset < length) {
        const ssize_t written = ::write(master_fd_, data + offset, length - offset);
        if (written > 0) {
            offset += static_cast<std::size_t>(written);
            continue;
        }
        if (written < 0 && errno == EINTR)
            continue;
        return false;
    }
    bytes_written_ += length;
    ++write_events_;
    return true;
#else
    (void)data;
    (void)length;
    return false;
#endif
}

bool PosixPtySession::Resize(unsigned int columns, unsigned int rows)
{
#if defined(SAKURA_WX_POSIX)
    std::lock_guard lock(io_mutex_);
    if (master_fd_ < 0)
        return false;

    struct winsize size {};
    size.ws_col = static_cast<unsigned short>(columns);
    size.ws_row = static_cast<unsigned short>(rows);
    const bool resized = ::ioctl(master_fd_, TIOCSWINSZ, &size) == 0;
    if (resized)
        ++resize_events_;
    return resized;
#else
    (void)columns;
    (void)rows;
    return false;
#endif
}

std::vector<std::string> PosixPtySession::TakeOutput()
{
    std::deque<std::string> pending;
    {
        std::lock_guard lock(output_mutex_);
        pending.swap(output_);
    }
    uint64_t bytes = 0;
    for (const auto& chunk : pending)
        bytes += chunk.size();
    queued_bytes_.fetch_sub(bytes);
    return std::vector<std::string>(pending.begin(), pending.end());
}

TransportMetrics PosixPtySession::GetMetrics() const
{
    return {
        bytes_read_.load(),
        read_events_.load(),
        bytes_written_.load(),
        write_events_.load(),
        resize_events_.load(),
        max_queued_bytes_.load(),
    };
}

TransportStatus PosixPtySession::GetStatus() const
{
    return {
        state_.load(),
        exit_code_.load(),
        exit_signal_.load(),
        exit_code_valid_.load(),
    };
}

void PosixPtySession::ReadLoop(int fd)
{
#if defined(SAKURA_WX_POSIX)
    char buffer[8192];
    while (!stop_requested_.load()) {
        struct pollfd descriptor {fd, POLLIN, 0};
        const int result = ::poll(&descriptor, 1, 100);
        if (result < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (result == 0)
            continue;
        if ((descriptor.revents & (POLLIN | POLLHUP | POLLERR)) == 0)
            continue;

        const ssize_t received = ::read(fd, buffer, sizeof(buffer));
        if (received > 0) {
            std::lock_guard lock(output_mutex_);
            output_.emplace_back(buffer, static_cast<std::size_t>(received));
            bytes_read_ += static_cast<uint64_t>(received);
            ++read_events_;
            const uint64_t queued = queued_bytes_.fetch_add(
                static_cast<uint64_t>(received)) + static_cast<uint64_t>(received);
            RecordMaximum(max_queued_bytes_, queued);
            continue;
        }
        if (received < 0 && errno == EINTR)
            continue;
        break;
    }
#else
    (void)fd;
#endif
#if defined(SAKURA_WX_POSIX)
    if (!stop_requested_.load()) {
        int child = -1;
        {
            std::lock_guard lock(io_mutex_);
            child = child_pid_;
        }
        if (child > 0) {
            for (;;) {
                int status = 0;
                const pid_t result = ::waitpid(static_cast<pid_t>(child), &status, WNOHANG);
                if (result == static_cast<pid_t>(child)) {
                    RecordExitStatus(exit_code_, exit_signal_, exit_code_valid_, status);
                    break;
                }
                if (result < 0 || stop_requested_.load())
                    break;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }
        state_.store(TransportState::Exited);
    }
#endif
    running_.store(false);
}

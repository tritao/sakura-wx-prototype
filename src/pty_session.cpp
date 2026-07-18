#include "pty_session.h"

#if defined(SAKURA_WX_POSIX)

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <limits>
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

constexpr uint64_t kMaximumQueuedOutputBytes = 1024u * 1024u;

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

void SignalProcessGroup(pid_t process, int signal)
{
    if (process <= 0)
        return;
    if (::kill(-process, signal) < 0 && errno == ESRCH)
        ::kill(process, signal);
}

void ResetChildSignalState()
{
    struct sigaction action {};
    action.sa_handler = SIG_DFL;
    sigemptyset(&action.sa_mask);
    const int signals[] = {
        SIGHUP, SIGINT, SIGQUIT, SIGTERM, SIGPIPE, SIGCHLD,
        SIGTSTP, SIGTTIN, SIGTTOU, SIGWINCH,
    };
    for (const int signal : signals)
        ::sigaction(signal, &action, nullptr);

    sigset_t mask;
    sigemptyset(&mask);
    ::sigprocmask(SIG_SETMASK, &mask, nullptr);
}

enum class WaitResult {
    Reaped,
    AlreadyReaped,
    TimedOut,
    Failed,
};

WaitResult WaitForChild(pid_t child, int& status,
                        std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        const pid_t result = ::waitpid(child, &status, WNOHANG);
        if (result == child)
            return WaitResult::Reaped;
        if (result < 0) {
            if (errno == EINTR)
                continue;
            return errno == ECHILD ? WaitResult::AlreadyReaped
                                   : WaitResult::Failed;
        }
        if (std::chrono::steady_clock::now() >= deadline)
            return WaitResult::TimedOut;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
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
    {
        std::lock_guard lock(output_mutex_);
        output_.clear();
    }
    queued_bytes_.store(0);
    state_.store(TransportState::Starting);
    exit_code_.store(-1);
    exit_signal_.store(0);
    exit_code_valid_.store(false);

#if defined(SAKURA_WX_POSIX)
    struct winsize size {};
    const unsigned int maximum_dimension =
        std::numeric_limits<unsigned short>::max();
    size.ws_col = static_cast<unsigned short>(std::min(
        std::max(columns, 1u), maximum_dimension));
    size.ws_row = static_cast<unsigned short>(std::min(
        std::max(rows, 1u), maximum_dimension));

    int master = -1;
    const pid_t child = forkpty(&master, nullptr, nullptr, &size);
    if (child < 0) {
        state_.store(TransportState::Failed);
        return false;
    }

    if (child == 0) {
        // A GUI process may inherit SIGINT/SIGQUIT as ignored from its
        // launching shell. Restore normal terminal-child signal semantics so
        // the PTY's VINTR byte (Ctrl-C) can signal the foreground process
        // group, including commands such as `yes`.
        ResetChildSignalState();
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
        child_reaped_ = false;
    }

    const int flags = ::fcntl(master, F_GETFL, 0);
    if (flags >= 0)
        ::fcntl(master, F_SETFL, flags | O_NONBLOCK);

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
    bool child_reaped = false;
    {
        std::lock_guard lock(io_mutex_);
        child = child_pid_;
        child_reaped = child_reaped_;
    }
    if (child > 0 && !child_reaped)
        SignalProcessGroup(static_cast<pid_t>(child), SIGHUP);

    if (reader_thread_.joinable())
        reader_thread_.join();

    int master = -1;
    {
        std::lock_guard lock(io_mutex_);
        master = master_fd_;
        master_fd_ = -1;
        child_pid_ = -1;
        child_reaped = child_reaped_;
    }
    if (master >= 0)
        ::close(master);

    if (child > 0 && !child_reaped) {
        int status = 0;
        WaitResult result = WaitForChild(static_cast<pid_t>(child), status,
                                         std::chrono::milliseconds(0));
        if (result == WaitResult::TimedOut) {
            SignalProcessGroup(static_cast<pid_t>(child), SIGTERM);
            result = WaitForChild(static_cast<pid_t>(child), status,
                                  std::chrono::milliseconds(500));
        }
        if (result == WaitResult::TimedOut) {
            SignalProcessGroup(static_cast<pid_t>(child), SIGKILL);
            result = WaitForChild(static_cast<pid_t>(child), status,
                                  std::chrono::seconds(2));
        }
        if (result == WaitResult::Reaped)
            RecordExitStatus(exit_code_, exit_signal_, exit_code_valid_, status);
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
    if (data == nullptr && length != 0)
        return false;
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
        if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd descriptor {master_fd_, POLLOUT, 0};
            const int result = ::poll(&descriptor, 1, 100);
            if (result < 0 && errno == EINTR)
                continue;
            if (result > 0 && (descriptor.revents & POLLOUT) != 0)
                continue;
            if (result == 0)
                continue;
        }
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

std::vector<std::string> PosixPtySession::TakeOutput(std::size_t max_bytes)
{
    std::deque<std::string> pending;
    std::size_t taken = 0;
    {
        std::lock_guard lock(output_mutex_);
        while (!output_.empty() && taken < max_bytes) {
            std::string& chunk = output_.front();
            const std::size_t available = max_bytes - taken;
            if (chunk.size() <= available) {
                taken += chunk.size();
                pending.emplace_back(std::move(chunk));
                output_.pop_front();
            } else {
                pending.emplace_back(chunk.data(), available);
                chunk.erase(0, available);
                taken += available;
            }
        }
    }
    queued_bytes_.fetch_sub(static_cast<uint64_t>(taken));
    return std::vector<std::string>(pending.begin(), pending.end());
}

void PosixPtySession::DiscardOutput()
{
    std::lock_guard lock(output_mutex_);
    output_.clear();
    queued_bytes_.store(0);
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
        const uint64_t queued = queued_bytes_.load();
        if (queued >= kMaximumQueuedOutputBytes) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        const std::size_t read_capacity = std::min<std::size_t>(
            sizeof(buffer), static_cast<std::size_t>(
                kMaximumQueuedOutputBytes - queued));
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

        const ssize_t received = ::read(fd, buffer, read_capacity);
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
        if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
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
                if (result < 0 && errno == EINTR)
                    continue;
                if (result == static_cast<pid_t>(child)) {
                    RecordExitStatus(exit_code_, exit_signal_, exit_code_valid_, status);
                    std::lock_guard lock(io_mutex_);
                    if (child_pid_ == child)
                        child_reaped_ = true;
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

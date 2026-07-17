#include "pty_session.h"

#if defined(SAKURA_WX_POSIX)

#include <cerrno>
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

#if defined(SAKURA_WX_POSIX)
    struct winsize size {};
    size.ws_col = static_cast<unsigned short>(columns);
    size.ws_row = static_cast<unsigned short>(rows);

    int master = -1;
    const pid_t child = forkpty(&master, nullptr, nullptr, &size);
    if (child < 0)
        return false;

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
    reader_thread_ = std::thread(&PosixPtySession::ReadLoop, this, master);
    return true;
#else
    (void)columns;
    (void)rows;
    (void)shell;
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
            ::waitpid(static_cast<pid_t>(child), &status, 0);
        }
    }
#else
    if (reader_thread_.joinable())
        reader_thread_.join();
#endif

    running_.store(false);
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
    running_.store(false);
}

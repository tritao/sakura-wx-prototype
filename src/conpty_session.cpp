#include "conpty_session.h"

#if defined(_WIN32)

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

namespace {

void RecordMaximum(std::atomic<uint64_t>& maximum, uint64_t value)
{
    uint64_t current = maximum.load();
    while (current < value &&
           !maximum.compare_exchange_weak(current, value)) {
    }
}

std::wstring ToWide(const std::string& value)
{
    if (value.empty())
        return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, value.data(),
                                           static_cast<int>(value.size()),
                                           nullptr, 0);
    if (length <= 0)
        return {};
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(),
                        static_cast<int>(value.size()), result.data(), length);
    return result;
}

std::wstring DefaultShell()
{
    wchar_t shell[MAX_PATH] {};
    const DWORD length = GetEnvironmentVariableW(L"ComSpec", shell, MAX_PATH);
    if (length > 0 && length < MAX_PATH)
        return std::wstring(shell, length);
    return L"C:\\Windows\\System32\\cmd.exe";
}

void RecordExitCode(std::atomic<int>& exit_code,
                    std::atomic<bool>& exit_code_valid,
                    HANDLE process)
{
    DWORD code = STILL_ACTIVE;
    if (process != nullptr && GetExitCodeProcess(process, &code) &&
        code != STILL_ACTIVE) {
        exit_code.store(static_cast<int>(code));
        exit_code_valid.store(true);
    }
}

} // namespace

#endif

ConPtySession::~ConPtySession()
{
    Stop();
}

bool ConPtySession::Start(unsigned int columns, unsigned int rows,
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

#if defined(_WIN32)
    SECURITY_ATTRIBUTES security_attributes {};
    security_attributes.nLength = sizeof(security_attributes);
    security_attributes.bInheritHandle = TRUE;

    HANDLE input_read = nullptr;
    HANDLE input_write = nullptr;
    HANDLE output_read = nullptr;
    HANDLE output_write = nullptr;
    if (!CreatePipe(&input_read, &input_write, &security_attributes, 0) ||
        !CreatePipe(&output_read, &output_write, &security_attributes, 0)) {
        if (input_read) CloseHandle(input_read);
        if (input_write) CloseHandle(input_write);
        if (output_read) CloseHandle(output_read);
        if (output_write) CloseHandle(output_write);
        state_.store(TransportState::Failed);
        return false;
    }

    HPCON pseudo_console = nullptr;
    const COORD size {
        static_cast<SHORT>(std::clamp(columns, 1u, 32767u)),
        static_cast<SHORT>(std::clamp(rows, 1u, 32767u)),
    };
    const HRESULT console_result = CreatePseudoConsole(
        size, input_read, output_write, 0, &pseudo_console);
    if (FAILED(console_result)) {
        CloseHandle(input_read);
        CloseHandle(output_write);
        CloseHandle(input_write);
        CloseHandle(output_read);
        state_.store(TransportState::Failed);
        return false;
    }

    SIZE_T attribute_size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_size);
    if (attribute_size == 0) {
        ClosePseudoConsole(pseudo_console);
        CloseHandle(input_read);
        CloseHandle(output_write);
        CloseHandle(input_write);
        CloseHandle(output_read);
        state_.store(TransportState::Failed);
        return false;
    }
    std::vector<unsigned char> attribute_buffer(attribute_size);
    auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(
        attribute_buffer.data());
    if (!InitializeProcThreadAttributeList(attributes, 1, 0, &attribute_size)) {
        ClosePseudoConsole(pseudo_console);
        CloseHandle(input_read);
        CloseHandle(output_write);
        CloseHandle(input_write);
        CloseHandle(output_read);
        state_.store(TransportState::Failed);
        return false;
    }
    if (!UpdateProcThreadAttribute(attributes, 0,
                                   PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
                                   pseudo_console, sizeof(pseudo_console),
                                   nullptr, nullptr)) {
        DeleteProcThreadAttributeList(attributes);
        ClosePseudoConsole(pseudo_console);
        CloseHandle(input_read);
        CloseHandle(output_write);
        CloseHandle(input_write);
        CloseHandle(output_read);
        state_.store(TransportState::Failed);
        return false;
    }

    std::wstring command = shell.empty() ? DefaultShell() : ToWide(shell);
    if (command.empty())
        command = DefaultShell();
    command = L"\"" + command + L"\"";
    std::vector<wchar_t> command_line(command.begin(), command.end());
    command_line.push_back(L'\0');

    STARTUPINFOEXW startup_info {};
    startup_info.StartupInfo.cb = sizeof(startup_info);
    startup_info.lpAttributeList = attributes;
    PROCESS_INFORMATION process_info {};
    const BOOL created = CreateProcessW(
        nullptr, command_line.data(), nullptr, nullptr, FALSE,
        EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr,
        &startup_info.StartupInfo, &process_info);
    DeleteProcThreadAttributeList(attributes);
    CloseHandle(input_read);
    CloseHandle(output_write);
    if (process_info.hThread != nullptr)
        CloseHandle(process_info.hThread);
    if (!created) {
        ClosePseudoConsole(pseudo_console);
        CloseHandle(input_write);
        CloseHandle(output_read);
        state_.store(TransportState::Failed);
        return false;
    }

    HANDLE process_job = CreateJobObjectW(nullptr, nullptr);
    if (process_job != nullptr) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION job_info {};
        job_info.BasicLimitInformation.LimitFlags =
            JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(process_job, JobObjectExtendedLimitInformation,
                                     &job_info, sizeof(job_info)) ||
            !AssignProcessToJobObject(process_job, process_info.hProcess)) {
            CloseHandle(process_job);
            process_job = nullptr;
        }
    }

    {
        std::lock_guard lock(io_mutex_);
        input_write_ = input_write;
        output_read_ = output_read;
        pseudo_console_ = pseudo_console;
        process_handle_ = process_info.hProcess;
        process_job_ = process_job;
    }
    stop_requested_.store(false);
    running_.store(true);
    state_.store(TransportState::Running);
    reader_thread_ = std::thread(&ConPtySession::ReadLoop, this, output_read);
    return true;
#else
    (void)columns;
    (void)rows;
    (void)shell;
    state_.store(TransportState::Failed);
    return false;
#endif
}

void ConPtySession::Stop()
{
#if defined(_WIN32)
    stop_requested_.store(true);

    HANDLE process = nullptr;
    HANDLE output = nullptr;
    HANDLE input = nullptr;
    HPCON pseudo_console = nullptr;
    HANDLE process_job = nullptr;
    {
        std::lock_guard lock(io_mutex_);
        process = static_cast<HANDLE>(process_handle_);
        output = static_cast<HANDLE>(output_read_);
        input = static_cast<HANDLE>(input_write_);
        pseudo_console = static_cast<HPCON>(pseudo_console_);
        process_job = static_cast<HANDLE>(process_job_);
        process_handle_ = nullptr;
        output_read_ = nullptr;
        input_write_ = nullptr;
        pseudo_console_ = nullptr;
        process_job_ = nullptr;
    }

    if (process_job != nullptr)
        TerminateJobObject(process_job, 0);
    if (process != nullptr)
        TerminateProcess(process, 0);
    if (output != nullptr)
        CancelIoEx(output, nullptr);
    if (reader_thread_.joinable())
        reader_thread_.join();
    if (output != nullptr)
        CloseHandle(output);
    if (input != nullptr)
        CloseHandle(input);
    if (pseudo_console != nullptr)
        ClosePseudoConsole(pseudo_console);
    if (process != nullptr) {
        WaitForSingleObject(process, 2000);
        if (!exit_code_valid_.load())
            RecordExitCode(exit_code_, exit_code_valid_, process);
        CloseHandle(process);
    }
    if (process_job != nullptr)
        CloseHandle(process_job);
#else
    if (reader_thread_.joinable())
        reader_thread_.join();
#endif
    running_.store(false);
    const TransportState state = state_.load();
    if (state == TransportState::Starting || state == TransportState::Running)
        state_.store(TransportState::Stopped);
}

bool ConPtySession::Write(const char* data, std::size_t length)
{
#if defined(_WIN32)
    std::lock_guard lock(io_mutex_);
    auto input = static_cast<HANDLE>(input_write_);
    if (input == nullptr)
        return false;

    std::size_t offset = 0;
    while (offset < length) {
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(
            length - offset, 1u << 20));
        DWORD written = 0;
        if (!WriteFile(input, data + offset, chunk, &written, nullptr) ||
            written == 0)
            return false;
        offset += written;
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

bool ConPtySession::Resize(unsigned int columns, unsigned int rows)
{
#if defined(_WIN32)
    std::lock_guard lock(io_mutex_);
    auto console = static_cast<HPCON>(pseudo_console_);
    if (console == nullptr)
        return false;
    const COORD size {
        static_cast<SHORT>(std::clamp(columns, 1u, 32767u)),
        static_cast<SHORT>(std::clamp(rows, 1u, 32767u)),
    };
    const bool resized = SUCCEEDED(ResizePseudoConsole(console, size));
    if (resized)
        ++resize_events_;
    return resized;
#else
    (void)columns;
    (void)rows;
    return false;
#endif
}

std::vector<std::string> ConPtySession::TakeOutput()
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

TransportMetrics ConPtySession::GetMetrics() const
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

TransportStatus ConPtySession::GetStatus() const
{
    return {
        state_.load(),
        exit_code_.load(),
        exit_signal_.load(),
        exit_code_valid_.load(),
    };
}

void ConPtySession::ReadLoop(void* output_handle)
{
#if defined(_WIN32)
    auto output = static_cast<HANDLE>(output_handle);
    char buffer[8192];
    while (!stop_requested_.load()) {
        DWORD received = 0;
        if (!ReadFile(output, buffer, sizeof(buffer), &received, nullptr) ||
            received == 0)
            break;
        {
            std::lock_guard lock(output_mutex_);
            output_.emplace_back(buffer, received);
            bytes_read_ += received;
            ++read_events_;
            const uint64_t queued = queued_bytes_.fetch_add(received) + received;
            RecordMaximum(max_queued_bytes_, queued);
        }
    }
#else
    (void)output_handle;
#endif
    if (!stop_requested_.load()) {
#if defined(_WIN32)
        HANDLE process = nullptr;
        {
            std::lock_guard lock(io_mutex_);
            process = static_cast<HANDLE>(process_handle_);
        }
        while (process != nullptr && !stop_requested_.load() &&
               WaitForSingleObject(process, 0) == WAIT_TIMEOUT) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if (!stop_requested_.load())
            RecordExitCode(exit_code_, exit_code_valid_, process);
        state_.store(TransportState::Exited);
#endif
    }
    running_.store(false);
}

#include <sakura/terminal/factory.h>

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {

void Check(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

TransportStatus WaitForTerminalState(TerminalTransport& transport)
{
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(5);
    TransportStatus status;
    while (std::chrono::steady_clock::now() < deadline) {
        status = transport.GetStatus();
        if (status.state == TransportState::Exited ||
            status.state == TransportState::Failed)
            return status;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return transport.GetStatus();
}

std::string DrainOutput(TerminalTransport& transport)
{
    std::string output;
    for (const auto& chunk : transport.TakeOutput())
        output += chunk;
    return output;
}

} // namespace

int main()
{
    try {
        auto transport = CreateTerminalTransport();
        Check(transport != nullptr, "transport factory returned null");
        const bool started = transport->Start(
            80, 24, "/this/shell/does/not/exist/sakura-lifecycle-test");
        const TransportStatus status = WaitForTerminalState(*transport);

#if defined(_WIN32)
        Check(!started && status.state == TransportState::Failed,
              "invalid Windows shell did not fail during startup");
#else
        Check(started && status.state == TransportState::Exited,
              "invalid POSIX shell did not produce an exited session");
        Check(status.exit_code_valid && status.exit_code == 127,
              "invalid POSIX shell did not report exec failure 127");
#endif

        transport->Stop();
        transport->Stop();

#if defined(_WIN32)
        const std::string old_command =
            "echo sakura-old-session\rexit\r";
        const std::string new_command =
            "echo sakura-new-session\rexit\r";
#else
        const std::string old_command =
            "printf 'sakura-old-session\\n'; exit\n";
        const std::string new_command =
            "printf 'sakura-new-session\\n'; exit\n";
#endif

        Check(transport->Start(80, 24, ""),
              "could not start the first restart session");
        Check(transport->Write(old_command.data(), old_command.size()),
              "could not write the first restart command");
        const TransportStatus old_status = WaitForTerminalState(*transport);
        Check(old_status.state == TransportState::Exited,
              "first restart session did not exit");

        Check(transport->Start(80, 24, ""),
              "could not restart the transport");
        const std::string stale_output = DrainOutput(*transport);
        Check(stale_output.find("sakura-old-session") == std::string::npos,
              "restart exposed stale output from the previous session");
        Check(transport->Write(new_command.data(), new_command.size()),
              "could not write the second restart command");
        const TransportStatus new_status = WaitForTerminalState(*transport);
        Check(new_status.state == TransportState::Exited,
              "second restart session did not exit");
        const std::string new_output = DrainOutput(*transport);
        Check(new_output.find("sakura-new-session") != std::string::npos,
              "restart session output was not delivered");

        transport->Stop();
#if !defined(_WIN32)
        Check(transport->Start(80, 24, "/bin/sh"),
              "could not start the shutdown escalation session");
        const std::string stubborn_command =
            "trap '' HUP TERM; while :; do :; done\n";
        Check(transport->Write(stubborn_command.data(), stubborn_command.size()),
              "could not write the shutdown escalation command");
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        const auto stop_started = std::chrono::steady_clock::now();
        transport->Stop();
        const auto stop_elapsed = std::chrono::steady_clock::now() - stop_started;
        Check(stop_elapsed < std::chrono::seconds(5),
              "stubborn POSIX process was not stopped promptly");
        Check(!transport->IsRunning(),
              "stubborn POSIX process remained running after Stop");
#endif
        transport->Stop();
        std::cout << "transport_lifecycle: PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "transport_lifecycle: FAIL: " << error.what() << '\n';
        return 1;
    }
}

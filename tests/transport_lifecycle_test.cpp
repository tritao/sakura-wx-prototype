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
        std::cout << "transport_lifecycle: PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "transport_lifecycle: FAIL: " << error.what() << '\n';
        return 1;
    }
}

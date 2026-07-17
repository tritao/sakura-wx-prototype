#include <sakura/terminal/factory.h>

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

void Check(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

} // namespace

int main()
{
    try {
        auto transport = CreateTerminalTransport();
        Check(transport != nullptr, "transport factory returned null");
        Check(transport->Start(80, 24, ""), "transport failed to start");
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

#if defined(_WIN32)
        const std::string command = "echo transport-smoke\rexit\r";
#else
        const std::string command = "printf 'transport-smoke\\n'; exit\n";
#endif
        Check(transport->Write(command.data(), command.size()),
              "transport failed to write command");

        std::string output;
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < deadline) {
            for (const auto& chunk : transport->TakeOutput())
                output += chunk;
            if (output.find("transport-smoke") != std::string::npos &&
                transport->GetStatus().state == TransportState::Exited)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        const TransportStatus status = transport->GetStatus();
        const TransportMetrics metrics = transport->GetMetrics();
        transport->Stop();
        Check(output.find("transport-smoke") != std::string::npos,
              "transport output did not contain the marker");
        Check(status.state == TransportState::Exited &&
                  status.exit_code_valid && status.exit_code == 0,
              "transport did not report a clean process exit");
        Check(metrics.bytes_read > 0 && metrics.read_events > 0,
              "transport read metrics were not recorded");

        std::cout << "transport_smoke: PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "transport_smoke: FAIL: " << error.what() << '\n';
        return 1;
    }
}

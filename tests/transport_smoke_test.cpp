#include "transport_factory.h"

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

#if defined(_WIN32)
        const std::string command = "echo transport-smoke\r\nexit\r\n";
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
            if (output.find("transport-smoke") != std::string::npos)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        const TransportMetrics metrics = transport->GetMetrics();
        transport->Stop();
        Check(output.find("transport-smoke") != std::string::npos,
              "transport output did not contain the marker");
        Check(metrics.bytes_read > 0 && metrics.read_events > 0,
              "transport read metrics were not recorded");

        std::cout << "transport_smoke: PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "transport_smoke: FAIL: " << error.what() << '\n';
        return 1;
    }
}

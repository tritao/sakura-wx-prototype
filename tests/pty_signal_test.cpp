#include "pty_session.h"

#include <chrono>
#include <cstring>
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

bool WaitForMarker(PosixPtySession& session, std::string& output,
                   const std::string& marker)
{
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        for (const auto& chunk : session.TakeOutput())
            output += chunk;
        if (output.find(marker) != std::string::npos)
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    for (const auto& chunk : session.TakeOutput())
        output += chunk;
    return output.find(marker) != std::string::npos;
}

void RunSignalCase(unsigned char signal_byte, const char* marker,
                   const char* followup, const char* label,
                   bool expect_exit)
{
    PosixPtySession session;
    Check(session.Start(80, 24, "/bin/sh"),
          "could not start the POSIX PTY signal case");
    Check(session.Write("yes\n", 4), "could not start yes for signal case");

    std::string output;
    const auto output_deadline = std::chrono::steady_clock::now() +
                                 std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < output_deadline &&
           output.empty()) {
        for (const auto& chunk : session.TakeOutput())
            output += chunk;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    Check(!output.empty(), "signal case command did not produce output");
    // Wait for the shell to finish placing yes in the foreground process
    // group. Without this small synchronization window, a terminal signal
    // can race with the shell's job-control handoff and hit the shell itself.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    Check(session.Write(reinterpret_cast<const char*>(&signal_byte), 1),
          "could not send terminal signal byte");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    Check(session.Write(followup, std::strlen(followup)),
          "could not write signal follow-up command");
    Check(WaitForMarker(session, output, marker),
          "terminal signal did not return control to the shell");

    if (expect_exit) {
        const auto exit_deadline = std::chrono::steady_clock::now() +
                                   std::chrono::seconds(3);
        while (session.IsRunning() &&
               std::chrono::steady_clock::now() < exit_deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        if (session.IsRunning()) {
            for (const auto& chunk : session.TakeOutput())
                output += chunk;
            std::cerr << "pty_signal: " << label
                      << " did not exit; output tail: ";
            const std::size_t tail_start = output.size() > 4096
                ? output.size() - 4096 : 0;
            std::cerr.write(output.data() + tail_start,
                            static_cast<std::streamsize>(
                                output.size() - tail_start));
            std::cerr << '\n';
            throw std::runtime_error("signal case shell did not exit cleanly");
        }
    } else {
        Check(session.IsRunning(),
              "Ctrl-Z did not return control to a running shell");
    }
    session.Stop();
    Check(!session.IsRunning(), "signal case remained running after Stop");
}

} // namespace

int main()
{
    try {
        RunSignalCase(0x1c, "sakura-sigquit-ok",
                      "printf 'sakura-sigquit-%s\\n' ok; exit\n",
                      "SIGQUIT", true);
        RunSignalCase(0x1a, "sakura-sigtstp-ok",
                      "printf 'sakura-sigtstp-%s\\n' ok\n",
                      "SIGTSTP", false);
        std::cout << "pty_signal: PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "pty_signal: FAIL: " << error.what() << '\n';
        return 1;
    }
}

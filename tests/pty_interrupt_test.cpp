#include "pty_session.h"

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
        PosixPtySession session;
        Check(session.Start(80, 24, "/bin/sh"),
              "could not start the POSIX PTY");
        Check(session.Write("yes\n", 4), "could not start yes");

        std::string output;
        const auto output_deadline = std::chrono::steady_clock::now() +
                                      std::chrono::seconds(3);
        while (std::chrono::steady_clock::now() < output_deadline &&
               output.empty()) {
            for (const auto& chunk : session.TakeOutput())
                output += chunk;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        Check(!output.empty(), "yes did not produce PTY output");

        const TransportMetrics before_resize = session.GetMetrics();
        for (unsigned int index = 0; index < 8; ++index) {
            Check(session.Resize(40 + index * 7, 12 + index),
                  "resize failed while PTY output was flooding");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        const TransportMetrics saturated = session.GetMetrics();
        Check(saturated.max_queued_bytes > 0,
              "PTY output queue did not record any flooded output");
        Check(saturated.max_queued_bytes <= 1024u * 1024u,
              "PTY output queue exceeded its high-water limit");
        Check(saturated.resize_events >= before_resize.resize_events + 8,
              "resize events were lost during PTY output flooding");

        const auto bounded_drain = session.TakeOutput(1024);
        std::size_t bounded_bytes = 0;
        for (const auto& chunk : bounded_drain)
            bounded_bytes += chunk.size();
        Check(bounded_bytes <= 1024,
              "bounded PTY output drain exceeded its byte budget");
        session.DiscardOutput();

        const std::size_t bytes_before_interrupt = output.size();
        Check(session.Write("\x03", 1), "could not send Ctrl-C to the PTY");
        const std::string post_interrupt_command =
            "printf 'interrupt-ok\\n'; exit\n";
        Check(session.Write(post_interrupt_command.data(),
                            post_interrupt_command.size()),
              "could not write the post-interrupt command");

        const auto exit_deadline = std::chrono::steady_clock::now() +
                                   std::chrono::seconds(3);
        while (std::chrono::steady_clock::now() < exit_deadline) {
            for (const auto& chunk : session.TakeOutput())
                output += chunk;
            if (!session.IsRunning() &&
                output.find("interrupt-ok") != std::string::npos)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        for (const auto& chunk : session.TakeOutput())
            output += chunk;
        session.Stop();

        Check(output.find("interrupt-ok") != std::string::npos,
              "Ctrl-C did not return control to the shell");
        Check(output.size() < bytes_before_interrupt + 2 * 1024 * 1024,
              "PTY interrupt left an unbounded output tail");

        std::cout << "pty_interrupt: PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "pty_interrupt: FAIL: " << error.what() << '\n';
        return 1;
    }
}

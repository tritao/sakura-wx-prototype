#include "pty_session.h"

#include <chrono>
#include <cstddef>
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
        Check(session.Start(80, 24, "/bin/sh"), "could not start the POSIX PTY");

        const std::string command =
            "i=0; while [ $i -lt 10000 ]; do "
            "printf 'stress-line-%05d\\n' $i; i=$((i+1)); "
            "done; printf 'stress-complete\\n'; exit\n";
        Check(session.Write(command.data(), command.size()),
              "could not write stress command to the PTY");

        std::string output;
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::seconds(10);
        unsigned int resize_count = 0;
        while (std::chrono::steady_clock::now() < deadline) {
            for (const auto& chunk : session.TakeOutput())
                output += chunk;

            if ((resize_count++ % 4) == 0)
                session.Resize(40 + (resize_count % 80), 12 + (resize_count % 20));

            if (!session.IsRunning() && output.find("stress-complete") != std::string::npos)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }

        for (const auto& chunk : session.TakeOutput())
            output += chunk;
        session.Stop();

        Check(output.find("stress-line-00000") != std::string::npos,
              "PTY output did not contain the first stress line");
        Check(output.find("stress-line-09999") != std::string::npos,
              "PTY output did not contain the final stress line");
        Check(output.find("stress-complete") != std::string::npos,
              "PTY child did not complete cleanly");
        Check(!session.IsRunning(), "PTY remained running after Stop");

        std::cout << "pty_stress: PASS bytes=" << output.size()
                  << " resizes=" << resize_count << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "pty_stress: FAIL: " << error.what() << '\n';
        return 1;
    }
}

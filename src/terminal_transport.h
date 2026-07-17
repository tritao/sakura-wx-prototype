#pragma once

#include <cstddef>
#include <string>
#include <vector>

class TerminalTransport {
public:
    virtual ~TerminalTransport() = default;

    virtual bool Start(unsigned int columns, unsigned int rows,
                       const std::string& shell = {}) = 0;
    virtual void Stop() = 0;
    virtual bool Write(const char* data, std::size_t length) = 0;
    virtual bool Resize(unsigned int columns, unsigned int rows) = 0;
    virtual std::vector<std::string> TakeOutput() = 0;
    virtual bool IsRunning() const = 0;
};

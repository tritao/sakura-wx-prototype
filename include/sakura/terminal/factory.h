#pragma once

#include <sakura/terminal/transport.h>

#include <memory>

std::unique_ptr<TerminalTransport> CreateTerminalTransport();

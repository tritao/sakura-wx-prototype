#pragma once

#include "terminal_transport.h"

#include <memory>

std::unique_ptr<TerminalTransport> CreateTerminalTransport();

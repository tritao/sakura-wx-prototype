#include "transport_factory.h"

#if defined(_WIN32)
#include "conpty_session.h"
#else
#include "pty_session.h"
#endif

std::unique_ptr<TerminalTransport> CreateTerminalTransport()
{
#if defined(_WIN32)
    return std::make_unique<ConPtySession>();
#else
    return std::make_unique<PosixPtySession>();
#endif
}

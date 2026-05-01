/*
 * spiritty_telnet.h — minimal helpers for wiring Telnetty into a terminal.
 *
 * The core itself is transport-agnostic. This header lives separately so
 * the WASM build can omit it without dragging in BSD socket symbols.
 */
#ifndef SPIRITTY_TELNET_H
#define SPIRITTY_TELNET_H

#include "spiritty.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sp_telnet_session sp_telnet_session;

typedef struct {
    const char* host;
    uint16_t    port;
    const char* terminal_type;   /* default "xterm-256color" */
    bool        enable_naws;
    bool        enable_mccp2;
} sp_telnet_opts;

SPIRITTY_API sp_telnet_session* sp_telnet_open(const sp_telnet_opts* opts);
SPIRITTY_API void               sp_telnet_close(sp_telnet_session* s);

/* Pumps the socket once: reads bytes, dispatches IAC, forwards application
 * data into the attached terminal via sp_terminal_write. Returns false on
 * connection close or fatal error. */
SPIRITTY_API bool sp_telnet_pump(sp_telnet_session* s, sp_terminal* term);

/* Called when the user resizes the host window — emits a NAWS subneg. */
SPIRITTY_API void sp_telnet_notify_resize(sp_telnet_session* s,
                                          int32_t cols, int32_t rows);

/* Forwards keystrokes to the remote side. */
SPIRITTY_API void sp_telnet_send(sp_telnet_session* s,
                                 const uint8_t* data, size_t len);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* SPIRITTY_TELNET_H */

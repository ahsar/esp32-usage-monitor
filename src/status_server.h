#pragma once
#include <stdint.h>

// Tiny HTTP status server on port 80. The host (or an editor plugin) pings it
// with `?state=running|done|failed` to reflect agent activity on the panel.
//
// "Thinking" state lives here because the server is also pumped from inside the
// blocking HTTPS client (httpRequest calls handleHttpStatus), so the caller
// cannot own it without a shared global.

void statusServerBegin();

// Called once on the transition to state=running (e.g. to wake the screen).
void statusServerSetWakeCallback(void (*cb)());

bool     statusServerThinking();
uint32_t statusServerThinkingSince();
void     statusServerClearThinking();

// Implemented here, called both from the main loop and from httpRequest.
extern "C" void handleHttpStatus();

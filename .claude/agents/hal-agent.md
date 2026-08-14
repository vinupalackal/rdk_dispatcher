# HAL Interface Agent

Scope: HAL wrapper calls only (hal_wifi.c, hal_wan.c). Never touches
dispatcher event-routing logic in dispatcher_handlers.c.

Input: a function signature request (e.g. "wrap CcspHalWifiRadioReset with
return-code checking and CcspTraceError logging")
Output: a HAL wrapper function following existing error-handling pattern

Tools: Read, Edit (hal_*.c files only)
Model: sonnet

# /explain-handler

What: Explains the most recently written/edited dispatcher handler function.

Why: I keep re-asking "what event triggers this, what does it call, what can
fail" after every new handler -- this saves retyping the prompt.

How:
1. Look at the most recently edited function in dispatcher_handlers.c
2. Output:
   - Triggering sysevent/Netlink event
   - What CCSP/HAL calls it makes downstream
   - Failure modes (what happens if the HAL call fails)
   - Whether it's registered/deregistered correctly in dispatcher_init.c
3. Under 120 words.

Next step: none -- read-only explanation.

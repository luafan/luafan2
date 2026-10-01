# Event loop

`fan.loop()` runs the process event loop as a persistent service loop. It does
not return merely because the current event set is empty; this preserves the
v1/webase contract for long-running servers. Call `fan.loopbreak()` to request
shutdown. The callback form `fan.loop(fn, ...)` remains supported and starts
the callback coroutine before entering the loop.

The runtime regression coverage is in `tests/lua/test_coro.lua`, including
park/resume, concurrent sleeps, GC retention, and explicit `loopbreak()` exit.

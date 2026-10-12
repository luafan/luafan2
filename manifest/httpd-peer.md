# HTTPD remote peer information

Both `fan.httpd` backends expose the accepted client's network peer on the
request object:

- `req:remoteinfo()` returns `{ ip = string, port = number }`;
- `req.remoteip` returns the peer IP string;
- `req.remoteport` returns the peer port number.

When the peer address is unavailable, `remoteinfo()` and both fields return
`nil`. The pure-Lua backend obtains the values from `fan.tcp`'s
`conn:getpeername()`. The C backend copies the values from libevent's
`evhttp_connection_get_peer()` while the request is alive, so they remain safe
to read during handler execution and after request completion callbacks.

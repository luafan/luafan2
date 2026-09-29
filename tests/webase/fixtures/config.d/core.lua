-- Test fixture config: WEBROOT env var already carries an absolute path
-- pointing at fixtures/web, so `(WORKDIR or "") ..` is a no-op prefix.
webroot = (WORKDIR or "") .. (os.getenv("WEBROOT") or "web")

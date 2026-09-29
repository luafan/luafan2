service_host = os.getenv("SERVICE_HOST") or "0.0.0.0"
service_port = tonumber(os.getenv("SERVICE_PORT") or 2201)
debug = os.getenv("DEBUG") == "true"

#!/usr/bin/env python3
"""Serve the local ADC scope, or the live power dashboard with --metrics.

Usage: python3 tools/serve_scope.py [--metrics] [--port PORT]
No third-party packages, firmware CORS changes, or Internet access required.
"""
import argparse
import http.client
import json
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlsplit


SITE = Path(__file__).resolve().parents[1] / "scope"
MAX_RESPONSE_BYTES = 32768
ASSETS = {
    "/": ("index.html", "text/html; charset=utf-8"),
    "/app.mjs": ("app.mjs", "text/javascript; charset=utf-8"),
    "/samples.mjs": ("samples.mjs", "text/javascript; charset=utf-8"),
}


def board_address(value: str) -> tuple[str, int]:
    """Accept an HTTP board host, not arbitrary paths, credentials or schemes."""
    value = value.strip()
    if not value or any(char.isspace() for char in value):
        raise ValueError("Enter the board's IP address or hostname.")
    url = urlsplit(value if "://" in value else "http://" + value)
    if (url.scheme != "http" or not url.hostname or url.username is not None
            or url.password is not None or url.path not in ("", "/")
            or url.query or url.fragment):
        raise ValueError("Use an HTTP board address only, for example 192.168.0.158.")
    port = url.port if url.port is not None else 80
    if not 1 <= port <= 65535:
        raise ValueError("Invalid board port.")
    return url.hostname, port


def fetch_json(host: str, port: int, endpoint: str) -> bytes:
    if endpoint not in ("/samples", "/metrics"):
        raise ValueError("Unsupported board endpoint.")
    # HTTPConnection avoids environment proxy settings and does not follow
    # redirects. Never proxy OTA, arbitrary paths, or non-GET requests.
    connection = http.client.HTTPConnection(host, port, timeout=5)
    try:
        connection.request("GET", endpoint, headers={"Accept": "application/json"})
        response = connection.getresponse()
        if response.status != 200:
            raise ValueError(f"Board returned HTTP {response.status}. Check its address and {endpoint} firmware support.")
        data = response.read(MAX_RESPONSE_BYTES + 1)
        if len(data) > MAX_RESPONSE_BYTES:
            raise ValueError("Board response is too large.")
        try:
            json.loads(data)
        except (ValueError, UnicodeError) as error:
            raise ValueError(f"The board did not return valid JSON from {endpoint}.") from error
        return data
    finally:
        connection.close()


def fetch_samples(host: str, port: int) -> bytes:
    return fetch_json(host, port, "/samples")


def fetch_metrics(host: str, port: int) -> bytes:
    return fetch_json(host, port, "/metrics")


class ScopeHandler(BaseHTTPRequestHandler):
    site = SITE
    assets = ASSETS
    endpoint = "/samples"

    def fetch_data(self, host: str, port: int) -> bytes:
        return fetch_samples(host, port)

    def reply(self, status: int, body: bytes, content_type: str) -> None:
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.end_headers()
        self.wfile.write(body)

    def error(self, status: int, message: str) -> None:
        self.reply(status, json.dumps({"error": message}).encode(), "application/json")

    def do_GET(self) -> None:
        # Loopback binding plus Host/Origin checks keep other websites from
        # using this local-only proxy, including through DNS rebinding.
        hosts = {f"127.0.0.1:{self.server.server_port}", f"localhost:{self.server.server_port}"}
        host = self.headers.get("Host", "")
        if host not in hosts or self.headers.get("Origin", "http://" + host) != "http://" + host:
            self.error(403, "Open this site through its localhost URL.")
            return

        url = urlsplit(self.path)
        if url.path in self.assets:
            filename, content_type = self.assets[url.path]
            self.reply(200, (self.site / filename).read_bytes(), content_type)
        elif url.path == self.endpoint:
            query = parse_qs(url.query, keep_blank_values=True)
            try:
                if set(query) != {"board"} or len(query["board"]) != 1:
                    raise ValueError("Supply one board address.")
                host, port = board_address(query["board"][0])
            except ValueError as error:
                self.error(400, str(error))
                return
            try:
                data = self.fetch_data(host, port)
            except (OSError, http.client.HTTPException, ValueError) as error:
                self.error(502, f"Could not fetch {self.endpoint[1:]}: {error}")
                return
            self.reply(200, data, "application/json")
        else:
            self.error(404, "Not found")


class MetricsHandler(ScopeHandler):
    site = SITE.parent / "metrics"
    assets = {
        "/": ("index.html", "text/html; charset=utf-8"),
        "/app.mjs": ("app.mjs", "text/javascript; charset=utf-8"),
        "/history.mjs": ("history.mjs", "text/javascript; charset=utf-8"),
    }
    endpoint = "/metrics"

    def fetch_data(self, host: str, port: int) -> bytes:
        return fetch_metrics(host, port)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--metrics", action="store_true", help="show W and VA over time instead of ADC samples")
    parser.add_argument("--port", type=int)
    args = parser.parse_args()
    port = args.port if args.port is not None else (8002 if args.metrics else 8000)
    handler = MetricsHandler if args.metrics else ScopeHandler
    title = "Power dashboard" if args.metrics else "ADC scope"
    with ThreadingHTTPServer(("127.0.0.1", port), handler) as server:
        print(f"{title}: http://127.0.0.1:{server.server_port} (Ctrl-C to stop)", flush=True)
        try:
            server.serve_forever()
        except KeyboardInterrupt:
            pass


if __name__ == "__main__":
    main()

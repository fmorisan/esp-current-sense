import http.client
import json
import threading
import unittest
from http.server import ThreadingHTTPServer
from unittest.mock import Mock, patch

from tools.serve_scope import (
    MAX_RESPONSE_BYTES, MetricsHandler, ScopeHandler, board_address, fetch_json,
    fetch_metrics, fetch_samples,
)


class BoardAddressTests(unittest.TestCase):
    def test_addresses(self):
        for value, expected in [
            ("192.168.0.158", ("192.168.0.158", 80)),
            ("http://esp-current-sense.local/", ("esp-current-sense.local", 80)),
            ("192.168.0.158:8080", ("192.168.0.158", 8080)),
            ("http://[::1]:8080", ("::1", 8080)),
        ]:
            with self.subTest(value=value):
                self.assertEqual(board_address(value), expected)

    def test_rejects_non_board_urls(self):
        for value in ["", "http://", "https://board", "ftp://board", "board/samples",
                      "board?x=1", "board#test", "user:pass@board", "board:0",
                      "board:65536", "board:invalid", "board with spaces"]:
            with self.subTest(value=value), self.assertRaises(ValueError):
                board_address(value)


class FetchTests(unittest.TestCase):
    def test_metrics_uses_the_metrics_endpoint(self):
        with patch("tools.serve_scope.fetch_json", return_value=b"{}") as fetch:
            self.assertEqual(fetch_metrics("board", 80), b"{}")
            fetch.assert_called_once_with("board", 80, "/metrics")

    def test_arbitrary_endpoints_never_open_connections(self):
        with patch("tools.serve_scope.http.client.HTTPConnection") as connection:
            with self.assertRaises(ValueError):
                fetch_json("board", 80, "/update")
            connection.assert_not_called()

    def test_fetches_only_get_samples_and_closes_connection(self):
        response = Mock(status=200)
        response.read.return_value = b'{"voltage_mv":[1650],"current_mv":[1651.25]}'
        with patch("tools.serve_scope.http.client.HTTPConnection") as connection_class:
            connection = connection_class.return_value
            connection.getresponse.return_value = response
            self.assertEqual(fetch_samples("board", 80), response.read.return_value)
            connection_class.assert_called_once_with("board", 80, timeout=5)
            connection.request.assert_called_once_with(
                "GET", "/samples", headers={"Accept": "application/json"})
            response.read.assert_called_once_with(MAX_RESPONSE_BYTES + 1)
            connection.close.assert_called_once()

    def test_rejects_redirects_errors_oversized_and_non_json_responses(self):
        for status, data in [(302, b""), (404, b"not found"), (200, b"not JSON"),
                             (200, b"x" * (MAX_RESPONSE_BYTES + 1))]:
            with self.subTest(status=status, length=len(data)):
                with patch("tools.serve_scope.http.client.HTTPConnection") as connection_class:
                    connection = connection_class.return_value
                    connection.getresponse.return_value = Mock(status=status)
                    connection.getresponse.return_value.read.return_value = data
                    with self.assertRaises(ValueError):
                        fetch_samples("board", 80)
                    connection.close.assert_called_once()


class QuietHandler(ScopeHandler):
    def log_message(self, *args):
        pass


class LocalSiteTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.server = ThreadingHTTPServer(("127.0.0.1", 0), QuietHandler)
        cls.thread = threading.Thread(target=cls.server.serve_forever, daemon=True)
        cls.thread.start()

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()
        cls.thread.join()

    def request(self, path, headers=None):
        connection = http.client.HTTPConnection("127.0.0.1", self.server.server_port, timeout=2)
        try:
            connection.request("GET", path, headers=headers or {})
            response = connection.getresponse()
            return response.status, dict(response.getheaders()), response.read()
        finally:
            connection.close()

    def test_serves_only_viewer_assets(self):
        for path in ["/", "/app.mjs", "/samples.mjs"]:
            with self.subTest(path=path):
                status, headers, body = self.request(path)
                self.assertEqual(status, 200)
                self.assertEqual(headers["Cache-Control"], "no-store")
                self.assertTrue(body)
        for path in ["/firmware/secrets.h", "/../firmware/secrets.h", "/update", "/README.md"]:
            with self.subTest(path=path):
                self.assertEqual(self.request(path)[0], 404)

    def test_proxies_snapshot(self):
        payload = b'{"voltage_mv":[1650],"current_mv":[1651.25]}'
        with patch("tools.serve_scope.fetch_samples", return_value=payload) as fetch:
            status, headers, body = self.request("/samples?board=192.168.0.158")
            self.assertEqual(status, 200)
            self.assertEqual(body, payload)
            self.assertEqual(headers["Content-Type"], "application/json")
            fetch.assert_called_once_with("192.168.0.158", 80)

    def test_invalid_queries_never_contact_board(self):
        with patch("tools.serve_scope.fetch_samples") as fetch:
            for path in ["/samples", "/samples?board=", "/samples?board=x&board=y",
                         "/samples?board=board/update", "/samples?board=x&path=/update"]:
                with self.subTest(path=path):
                    self.assertEqual(self.request(path)[0], 400)
            fetch.assert_not_called()

    def test_board_failures_are_visible(self):
        with patch("tools.serve_scope.fetch_samples", side_effect=TimeoutError("timed out")):
            status, _, body = self.request("/samples?board=board")
            self.assertEqual(status, 502)
            self.assertIn("timed out", json.loads(body)["error"])

    def test_rejects_foreign_origins_and_hosts(self):
        with patch("tools.serve_scope.fetch_samples") as fetch:
            for headers in [{"Host": "attacker.example"}, {"Origin": "https://attacker.example"}]:
                with self.subTest(headers=headers):
                    self.assertEqual(self.request("/samples?board=board", headers)[0], 403)
            fetch.assert_not_called()


class QuietMetricsHandler(MetricsHandler):
    def log_message(self, *args):
        pass


class MetricsSiteTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.server = ThreadingHTTPServer(("127.0.0.1", 0), QuietMetricsHandler)
        cls.thread = threading.Thread(target=cls.server.serve_forever, daemon=True)
        cls.thread.start()

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()
        cls.thread.join()

    def request(self, path, headers=None):
        connection = http.client.HTTPConnection("127.0.0.1", self.server.server_port, timeout=2)
        try:
            connection.request("GET", path, headers=headers or {})
            response = connection.getresponse()
            return response.status, dict(response.getheaders()), response.read()
        finally:
            connection.close()

    def test_metrics_assets_and_endpoint(self):
        for path in ["/", "/app.mjs", "/history.mjs"]:
            with self.subTest(path=path):
                status, headers, body = self.request(path)
                self.assertEqual(status, 200)
                self.assertEqual(headers["Cache-Control"], "no-store")
                self.assertTrue(body)
        payload = b'{"power_w":910.0,"power_va":926.2}'
        with patch("tools.serve_scope.fetch_metrics", return_value=payload) as fetch:
            status, _, body = self.request("/metrics?board=192.168.0.158")
            self.assertEqual(status, 200)
            self.assertEqual(body, payload)
            fetch.assert_called_once_with("192.168.0.158", 80)

    def test_metrics_rejects_other_routes_origins_and_bad_queries(self):
        with patch("tools.serve_scope.fetch_metrics") as fetch:
            for path in ["/samples?board=board", "/update", "/firmware/secrets.h"]:
                with self.subTest(path=path):
                    self.assertEqual(self.request(path)[0], 404)
            self.assertEqual(self.request("/metrics")[0], 400)
            self.assertEqual(self.request("/metrics?board=board", {"Origin": "https://example.com"})[0], 403)
            fetch.assert_not_called()

    def test_metrics_reports_upstream_errors(self):
        with patch("tools.serve_scope.fetch_metrics", side_effect=TimeoutError("timed out")):
            status, _, body = self.request("/metrics?board=board")
            self.assertEqual(status, 502)
            self.assertIn("Could not fetch metrics", json.loads(body)["error"])


if __name__ == "__main__":
    unittest.main()

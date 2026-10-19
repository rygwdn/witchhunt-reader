#!/usr/bin/env python3
"""Smoke tests for the local OPDS firmware distributor."""

from __future__ import annotations

import sys
import tempfile
import threading
import unittest
from pathlib import Path
from urllib.error import HTTPError
from urllib.request import Request, urlopen

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from scripts.opds_firmware_server import FirmwareArtifact, FirmwareCatalog, create_server


class OpdsFirmwareServerTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temp_dir = tempfile.TemporaryDirectory()
        self.firmware_path = Path(self.temp_dir.name) / "firmware.bin"
        self.firmware = bytes(range(256)) * 32
        self.firmware_path.write_bytes(self.firmware)

        artifact = FirmwareArtifact.from_path(
            self.firmware_path,
            board="x4pro",
            version="2.31-test",
            title="X4 Pro & test firmware",
        )
        self.catalog = FirmwareCatalog([artifact])
        self.artifact = artifact
        self.server = create_server("127.0.0.1", 0, self.catalog)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.base_url = f"http://127.0.0.1:{self.server.server_port}"

    def tearDown(self) -> None:
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=2)
        self.temp_dir.cleanup()

    def test_catalog_advertises_firmware_acquisition(self) -> None:
        with urlopen(self.base_url + "/opds", timeout=2) as response:
            body = response.read().decode("utf-8")

        self.assertIn("X4 Pro &amp; test firmware", body)
        self.assertIn("application/vnd.crosspoint.firmware", body)
        self.assertIn(f'href="{self.artifact.route}"', body)
        self.assertIn(f'length="{len(self.firmware)}"', body)
        self.assertIn(self.artifact.sha256, body)

    def test_firmware_download_is_limited_to_configured_file(self) -> None:
        with urlopen(self.base_url + self.artifact.route, timeout=2) as response:
            self.assertEqual(response.status, 200)
            self.assertEqual(response.headers["Content-Type"], "application/octet-stream")
            self.assertEqual(int(response.headers["Content-Length"]), len(self.firmware))
            self.assertEqual(response.read(), self.firmware)

        with self.assertRaises(HTTPError) as error:
            urlopen(self.base_url + "/firmware/../firmware.bin", timeout=2)
        try:
            self.assertEqual(error.exception.code, 404)
        finally:
            error.exception.close()

    def test_head_reports_catalog_and_firmware_sizes(self) -> None:
        for path, expected_size in (("/opds", len(self.catalog.feed)), (self.artifact.route, len(self.firmware))):
            request = Request(self.base_url + path, method="HEAD")
            with urlopen(request, timeout=2) as response:
                self.assertEqual(response.status, 200)
                self.assertEqual(int(response.headers["Content-Length"]), expected_size)
                self.assertEqual(response.read(), b"")


if __name__ == "__main__":
    unittest.main()

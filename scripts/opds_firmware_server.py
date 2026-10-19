#!/usr/bin/env python3
"""Serve local CrossPoint firmware as a small OPDS catalog.

The server exposes only the configured .bin files and the catalog feed; it does
not provide a directory listing or arbitrary filesystem access. Bind it to a
Tailscale address, or to 0.0.0.0 when the host firewall/Tailscale ACLs provide
the access boundary:

  python scripts/opds_firmware_server.py \
      --firmware .pio/build/x4pro/firmware.bin \
      --version 2.31-dev \
      --host 0.0.0.0 --port 8080

Configure the reader's OPDS server URL as:

  http://<tailscale-host>:8080/opds

The firmware acquisition uses the vendor MIME type understood by the reader:
application/vnd.crosspoint.firmware. Selecting the entry downloads the image
and hands it to the existing firmware validation/confirmation flow.
"""

from __future__ import annotations

import argparse
import hashlib
import logging
import re
from dataclasses import dataclass
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Iterable
from urllib.parse import quote, urlsplit
from xml.sax.saxutils import escape

FIRMWARE_MIME = "application/vnd.crosspoint.firmware"
CATALOG_MIME = "application/atom+xml; charset=utf-8"
FIRMWARE_ROUTE_PREFIX = "/firmware/"


_BOARD_LABELS = {
    "x3": "X3",
    "x4": "X4",
    "x4pro": "X4 Pro",
    "lilygo": "LilyGo",
}


def _board_label(board: str) -> str:
    return _BOARD_LABELS.get(board.lower(), board.upper())


def _route_component(value: str) -> str:
    sanitized = re.sub(r"[^A-Za-z0-9._-]+", "_", value).strip("._")
    return sanitized or "firmware"


def _normalize_catalog_path(value: str) -> str:
    path = "/" + value.strip().strip("/")
    if path == "/":
        return path
    if any(segment in {".", ".."} for segment in path.split("/")):
        raise ValueError("catalog path must not contain '.' or '..' segments")
    return path


def _xml_attr(value: object) -> str:
    return escape(str(value), {"\"": "&quot;"})


@dataclass(frozen=True)
class FirmwareArtifact:
    path: Path
    board: str
    version: str
    title: str
    route: str
    size: int
    sha256: str

    @classmethod
    def from_path(
        cls,
        path: str | Path,
        *,
        board: str | None = None,
        version: str = "",
        title: str | None = None,
    ) -> "FirmwareArtifact":
        resolved = Path(path).expanduser().resolve()
        if not resolved.is_file():
            raise ValueError(f"firmware file does not exist: {resolved}")
        if resolved.suffix.lower() != ".bin":
            raise ValueError(f"firmware file must have a .bin extension: {resolved}")

        board_name = board or resolved.parent.name or "firmware"
        board_name = board_name.strip() or "firmware"
        version_label = version.strip()
        display_title = title.strip() if title else ""
        if not display_title:
            display_title = f"{_board_label(board_name)} firmware"
            if version_label:
                display_title += f" {version_label}"

        route_name = f"{_route_component(board_name)}-{_route_component(resolved.name)}"
        digest = hashlib.sha256()
        size = 0
        with resolved.open("rb") as firmware:
            for chunk in iter(lambda: firmware.read(1024 * 1024), b""):
                digest.update(chunk)
                size += len(chunk)

        return cls(
            path=resolved,
            board=board_name,
            version=version_label,
            title=display_title,
            route=FIRMWARE_ROUTE_PREFIX + quote(route_name, safe="-_.~"),
            size=size,
            sha256=digest.hexdigest(),
        )


class FirmwareCatalog:
    def __init__(self, artifacts: Iterable[FirmwareArtifact], catalog_path: str = "/opds") -> None:
        self.artifacts = tuple(artifacts)
        if not self.artifacts:
            raise ValueError("at least one firmware artifact is required")
        self.catalog_path = _normalize_catalog_path(catalog_path)
        self._artifacts_by_route = {artifact.route: artifact for artifact in self.artifacts}
        if len(self._artifacts_by_route) != len(self.artifacts):
            raise ValueError("firmware artifacts produce duplicate download routes")
        self.feed = self._render_feed().encode("utf-8")

    def artifact_for(self, route: str) -> FirmwareArtifact | None:
        return self._artifacts_by_route.get(route)

    def _render_feed(self) -> str:
        updated = datetime.now(timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z")
        entries: list[str] = []
        for artifact in self.artifacts:
            version = artifact.version or "local build"
            summary = (
                f"Board: {artifact.board}; Version: {version}; Size: {artifact.size} bytes; "
                f"SHA-256: {artifact.sha256}"
            )
            entries.append(
                "  <entry>\n"
                f"    <title>{escape(artifact.title)}</title>\n"
                f"    <id>urn:crosspoint:firmware:{artifact.sha256[:16]}</id>\n"
                "    <author><name>CrossPoint Reader</name></author>\n"
                f"    <summary>{escape(summary)}</summary>\n"
                f'    <link rel="http://opds-spec.org/acquisition" type="{FIRMWARE_MIME}" '
                f'href="{_xml_attr(artifact.route)}" length="{artifact.size}" />\n'
                "  </entry>"
            )

        return (
            '<?xml version="1.0" encoding="utf-8"?>\n'
            '<feed xmlns="http://www.w3.org/2005/Atom">\n'
            "  <title>CrossPoint Firmware</title>\n"
            "  <id>urn:crosspoint:firmware</id>\n"
            f"  <updated>{updated}</updated>\n"
            f'  <link rel="self" type="application/atom+xml" href="{_xml_attr(self.catalog_path)}" />\n'
            + "\n".join(entries)
            + "\n</feed>\n"
        )


class _FirmwareRequestHandler(BaseHTTPRequestHandler):
    server_version = "CrossPointFirmwareOPDS/1.0"

    @property
    def catalog_server(self) -> "FirmwareHTTPServer":
        return self.server  # type: ignore[return-value]

    def do_GET(self) -> None:
        self._dispatch(send_body=True)

    def do_HEAD(self) -> None:
        self._dispatch(send_body=False)

    def _dispatch(self, *, send_body: bool) -> None:
        request_path = urlsplit(self.path).path
        catalog = self.catalog_server.catalog
        if request_path == "/" and catalog.catalog_path != "/":
            self.send_response(302)
            self.send_header("Location", catalog.catalog_path)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return

        if request_path in {catalog.catalog_path, catalog.catalog_path + "/"}:
            self._send_bytes(catalog.feed, CATALOG_MIME, send_body=send_body)
            return

        artifact = catalog.artifact_for(request_path)
        if artifact is None:
            self.send_error(404, "Not found")
            return

        try:
            current_size = artifact.path.stat().st_size
            firmware = artifact.path.open("rb")
        except OSError as error:
            self.send_error(404, f"Firmware unavailable: {error}")
            return

        safe_filename = _route_component(artifact.path.name)
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(current_size))
        self.send_header("Content-Disposition", f'attachment; filename="{safe_filename}"')
        self.end_headers()
        if send_body:
            with firmware:
                while chunk := firmware.read(1024 * 1024):
                    self.wfile.write(chunk)
        else:
            firmware.close()

    def _send_bytes(self, payload: bytes, content_type: str, *, send_body: bool) -> None:
        self.send_response(200)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        if send_body:
            self.wfile.write(payload)


class FirmwareHTTPServer(ThreadingHTTPServer):
    allow_reuse_address = True
    daemon_threads = True

    def __init__(self, address: tuple[str, int], catalog: FirmwareCatalog) -> None:
        self.catalog = catalog
        super().__init__(address, _FirmwareRequestHandler)


def create_server(host: str, port: int, catalog: FirmwareCatalog) -> FirmwareHTTPServer:
    """Create a server separately from serving it, which keeps smoke tests offline."""

    return FirmwareHTTPServer((host, port), catalog)


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--firmware",
        action="append",
        required=True,
        metavar="PATH",
        help="local .bin image to publish; repeat for multiple board images",
    )
    parser.add_argument("--host", default="0.0.0.0", help="bind address (default: 0.0.0.0)")
    parser.add_argument("--port", type=int, default=8080, help="HTTP port (default: 8080)")
    parser.add_argument("--catalog-path", default="/opds", help="catalog URL path (default: /opds)")
    parser.add_argument("--board", help="board name for a single image; otherwise inferred from its parent directory")
    parser.add_argument("--version", default="", help="version shown in the catalog entry")
    parser.add_argument("--title", help="entry title for a single image")
    return parser


def _artifacts_from_args(args: argparse.Namespace) -> list[FirmwareArtifact]:
    if args.board and len(args.firmware) != 1:
        raise ValueError("--board can only be used with one --firmware argument")
    if args.title and len(args.firmware) != 1:
        raise ValueError("--title can only be used with one --firmware argument")
    return [
        FirmwareArtifact.from_path(
            path,
            board=args.board,
            version=args.version,
            title=args.title,
        )
        for path in args.firmware
    ]


def main() -> int:
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(message)s")
    args = _build_parser().parse_args()
    try:
        catalog = FirmwareCatalog(_artifacts_from_args(args), args.catalog_path)
    except ValueError as error:
        _build_parser().error(str(error))

    with create_server(args.host, args.port, catalog) as server:
        print(f"OPDS URL: http://<tailscale-host>:{server.server_port}{catalog.catalog_path}", flush=True)
        for artifact in catalog.artifacts:
            print(
                f"Serving {artifact.title}: {artifact.path} ({artifact.size} bytes, sha256={artifact.sha256})",
                flush=True,
            )
        try:
            server.serve_forever()
        except KeyboardInterrupt:
            print("", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

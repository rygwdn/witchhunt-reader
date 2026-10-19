#!/usr/bin/env python3
"""An offline BookOrbit catalog + kosync server for exercising the firmware's
BookOrbit Library on a real device, over plain HTTP with no TLS and no account.

lib/BookOrbitCatalog speaks BookOrbit's KOReader plugin API
(server/src/modules/koreader/koreader-{plugin,catalog}.controller.ts in the
BookOrbit repo): the kosync endpoints plus /koreader/plugin/version,
/koreader/plugin/catalog/*, all guarded by the x-auth-user/x-auth-key headers.
Reproducing a browsing session against the real server needed an account with
books in it. This removes that need.

Usage (see docs/bookorbit-integration-plan.md):

  python scripts/bookorbit_mock_server.py --host 0.0.0.0 --port 8080

Then on the device, Settings -> System -> KOReader Sync -> Sync Server URL:
  http://<your-lan-ip>:8080
with any username/password. "Authenticate" probes /koreader/plugin/version,
finds the catalog capability, and the Home screen gains a "BookOrbit Library"
row. Every request is logged, so you can watch exactly what the firmware sends.

The in-memory library holds a handful of books (including one with a long
description that overflows a 512-byte JSON token, and one with no cover), so
browsing, search, cover thumbnails, read-status/rating writes and file downloads
can all be exercised end to end.
"""

from __future__ import annotations

import argparse
import base64
import json
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

CATALOG_BASE = "/api/v1/koreader/plugin/catalog"

# A valid 1x1 JPEG served as every thumbnail, so the device's cover-decode path
# is exercised without shipping real artwork.
TINY_JPEG = base64.b64decode(
    "/9j/4AAQSkZJRgABAQEAYABgAAD/2wBDAAgGBgcGBQgHBwcJCQgKDBQNDAsLDBkSEw8UHRofHh0a"
    "HBwgJC4nICIsIxwcKDcpLDAxNDQ0Hyc5PTgyPC4zNDL/wAALCAABAAEBAREA/8QAFAABAAAAAAAA"
    "AAAAAAAAAAAACf/EABQQAQAAAAAAAAAAAAAAAAAAAAD/2gAIAQEAAD8AVN//2Q=="
)

BOOKS = [

    {
        "id": 412,
        "title": "Leviathan Wakes",
        "authors": ["James S. A. Corey", "Daniel Abraham"],
        "seriesId": 9,
        "seriesName": "The Expanse",
        "seriesIndex": "1",
        "progressPercentage": 42.6,
        "readStatus": "reading",
        "formats": ["epub", "pdf"],
        "hasCover": True,
        "rating": 4,
        "pageCount": 577,
        "publishedYear": 2011,
        "publisher": "Orbit",
        "description": (
            "Humanity has colonized thousands of worlds beyond Earth, but when the crew of the ice-hauler "
            "Canterbury answers a distress call, a conspiracy spanning the inner planets and the Belt begins to "
            "unravel — one that could ignite a war and wake something far older than either side."
        ),
    },
    {
        "id": 77,
        "title": "Solaris",
        "authors": ["Stanislaw Lem"],
        "seriesId": None,
        "seriesName": None,
        "seriesIndex": None,
        "progressPercentage": None,
        "readStatus": None,
        "formats": ["epub"],
        "hasCover": False,
        "rating": None,
        "pageCount": 204,
        "publishedYear": 1961,
        "publisher": "",
        "description": "A planet-wide ocean that thinks.",
    },
    {
        "id": 91,
        "title": "A Memory Called Empire",
        "authors": ["Arkady Martine"],
        "seriesId": 12,
        "seriesName": "Teixcalaan",
        "seriesIndex": "1",
        "progressPercentage": 0,
        "readStatus": "want_to_read",
        "formats": ["epub"],
        "hasCover": True,
        "rating": None,
        "pageCount": 462,
        "publishedYear": 2019,
        "publisher": "Tor",
        # Long enough to overflow the firmware listing parser's 512-byte token
        # buffer if a server ever embedded it in a listing; the detail DOM parse
        # must carry it whole.
        "description": (
            "Ambassador Mahit Dzmare arrives in the center of the multi-system Teixcalaanli Empire carrying the "
            "imprints of her predecessor's memories — and discovers he is dead, the empire's poets are "
            "entangled in court politics, and something is very wrong with the neural implant that was supposed "
            "to let her consult everything Yskandr knew about surviving as a small station's envoy among "
            "imperial stars. She must find out who killed him, keep her own mind her own, and negotiate for "
            "her station's continued independence, all while falling in love with the culture that could "
            "erase hers, one dazzling poem and three-syllable name at a time, in a city where information "
            "is the sharpest weapon anyone carries and every friend may be an informer."
        ),
    },
]


def file_payload(book_id: int, fmt: str) -> bytes:
    base = f"Mock {fmt.upper()} contents for book {book_id}.\n".encode()
    return base * 64


def book_summary(book: dict) -> dict:
    return {
        "id": book["id"],
        "title": book["title"],
        "authors": book["authors"],
        "seriesId": book["seriesId"],
        "seriesName": book["seriesName"],
        "seriesIndex": book["seriesIndex"],
        "progressPercentage": book["progressPercentage"],
        "lastReadAt": None,
        "readStatus": book["readStatus"],
        "formats": book["formats"],
        "hasCover": book["hasCover"],
        "thumbnailUrl": f"{CATALOG_BASE}/books/{book['id']}/thumbnail" if book["hasCover"] else None,
        "detailUrl": f"{CATALOG_BASE}/books/{book['id']}",
        "addedAt": "2026-08-01T10:00:00.000Z",
        "updatedAt": "2026-09-01T10:00:00.000Z",
    }


def book_detail(book: dict) -> dict:
    detail = book_summary(book)
    detail.update(
        {
            "subtitle": None,
            "description": book["description"],
            "publisher": book["publisher"],
            "publishedDate": None,
            "publishedYear": book["publishedYear"],
            "language": "en",
            "isbn10": None,
            "isbn13": None,
            "libraryId": 1,
            "libraryName": "Main",
            "rating": book["rating"],
            "pageCount": book["pageCount"],
            "collections": [],
            "genres": [],
            "tags": [],
            "progress": None,
            "files": [
                {
                    "id": book["id"] * 10 + index,
                    "format": fmt,
                    "role": "content",
                    "sizeBytes": len(file_payload(book["id"], fmt)),
                    "durationSeconds": None,
                    "downloadUrl": f"{CATALOG_BASE}/files/{book['id'] * 10 + index}/download",
                    "devicePath": f"/books/{book['id']}/file.{fmt}",
                }
                for index, fmt in enumerate(book["formats"])
            ],
            "relatedSections": [],
        }
    )
    return detail


ROOT_SECTIONS = [
    {"id": "continue-reading", "title": "Continue reading", "section": "continue-reading",
     "booksHref": f"{CATALOG_BASE}/books?sort=recently_read&readStatus=reading"},
    {"id": "recent", "title": "Recently added", "section": "recent",
     "booksHref": f"{CATALOG_BASE}/books?sort=recently_added"},
    {"id": "libraries", "title": "Libraries", "section": "libraries",
     "href": f"{CATALOG_BASE}/sections/libraries"},
    {"id": "collections", "title": "Collections", "section": "collections",
     "href": f"{CATALOG_BASE}/sections/collections"},
    {"id": "smart-scopes", "title": "SmartScopes", "section": "smart-scopes",
     "href": f"{CATALOG_BASE}/sections/smart-scopes"},
    {"id": "authors", "title": "Authors", "section": "authors",
     "href": f"{CATALOG_BASE}/sections/authors"},
    {"id": "series", "title": "Series", "section": "series",
     "href": f"{CATALOG_BASE}/sections/series"},
    {"id": "all-books", "title": "All Books", "section": "all-books",
     "booksHref": f"{CATALOG_BASE}/books?sort=title"},
]


def parse_query(path: str) -> dict[str, list[str]]:
    from urllib.parse import parse_qs, urlsplit

    return parse_qs(urlsplit(path).query)


def books_page(query: dict) -> dict:
    size = int(query.get("size", ["20"])[0])
    page = int(query.get("page", ["1"])[0])
    q = (query.get("q", [""])[0] or "").lower()
    sort = query.get("sort", ["recently_added"])[0]
    read_status = query.get("readStatus", [None])[0]

    items = list(BOOKS)
    if q:
        items = [b for b in items if q in b["title"].lower() or any(q in a.lower() for a in b["authors"])]
    if read_status:
        items = [b for b in items if b["readStatus"] == read_status]
    if sort in ("title", "recently_added"):
        items.sort(key=lambda b: b["title"] if sort == "title" else b["id"])

    total = len(items)
    start = (page - 1) * size
    chunk = items[start:start + size]
    result = {
        "items": [book_summary(b) for b in chunk],
        "total": total,
        "page": page,
        "size": size,
        "hasNext": start + size < total,
        "hasPrevious": page > 1,
    }

    def page_url(n: int) -> str:
        params = "&".join(f"{k}={v}" for k, vs in query.items() for v in vs if k not in ("page",)) + f"&page={n}"
        return f"{CATALOG_BASE}/books?{params}"

    result["nextUrl"] = page_url(page + 1) if result["hasNext"] else None
    result["previousUrl"] = page_url(page - 1) if result["hasPrevious"] else None
    return result


class Handler(BaseHTTPRequestHandler):
    server_version = "BookOrbitMock/1.0"

    def log_message(self, fmt, *args):  # one line per request, like kosync's mock
        print(f"[{time.strftime('%H:%M:%S')}] {self.address_string()} {fmt % args}", flush=True)

    # ---- auth ----
    def check_auth(self) -> bool:
        user = self.headers.get("x-auth-user")
        key = self.headers.get("x-auth-key")
        if not user or not key:
            self.send_json(401, {"message": "Missing KOReader credentials"})
            return False
        return True

    def send_json(self, status: int, payload) -> None:
        body = json.dumps(payload).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def send_bytes(self, status: int, data: bytes, content_type: str) -> None:
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    # ---- routing ----
    def do_GET(self) -> None:
        path = self.path.split("?")[0]
        query = parse_query(self.path)

        if path == "/api/v1/koreader/users/auth":
            if self.check_auth():
                self.send_json(200, {"authorized": "OK", "username": self.headers.get("x-auth-user")})
            return

        if path == "/api/v1/koreader/plugin/version":
            if not self.check_auth():
                return
            self.send_json(200, {
                "pluginVersion": "0.9.9-mock",
                "serverVersion": "1.0.0-mock",
                "capabilities": ["catalogBulkManifest", "catalogDashboardSections", "bookmarkSync"],
            })
            return

        if path.startswith(f"{CATALOG_BASE}/"):
            if not self.check_auth():
                return
            self.route_catalog(path, query)
            return

        self.send_json(404, {"message": f"no route: {path}"})

    def route_catalog(self, path: str, query: dict) -> None:
        if path == f"{CATALOG_BASE}/root":
            self.send_json(200, {"sections": ROOT_SECTIONS})
            return

        if path == f"{CATALOG_BASE}/dashboard":
            self.send_json(200, {
                "generatedAt": "2026-09-17T00:00:00.000Z",
                "username": self.headers.get("x-auth-user"),
                "displayName": "Mock User",
                "totalBooks": len(BOOKS),
                "sections": ROOT_SECTIONS,
                "continueReading": [book_summary(b) for b in BOOKS if b["readStatus"] == "reading"],
                "discover": [book_summary(b) for b in BOOKS[:2]],
                "readingGoal": {},
                "readingStreak": {},
                "highlightOfTheDay": None,
            })
            return

        if path == f"{CATALOG_BASE}/books":
            self.send_json(200, books_page(query))
            return

        if path.startswith(f"{CATALOG_BASE}/books/"):
            rest = path[len(f"{CATALOG_BASE}/books/"):]
            if rest.endswith("/thumbnail"):
                book_id = int(rest[: -len("/thumbnail")])
                book = next((b for b in BOOKS if b["id"] == book_id), None)
                if book and book["hasCover"]:
                    self.send_bytes(200, TINY_JPEG, "image/jpeg")
                else:
                    self.send_json(404, {"message": "No thumbnail"})
                return
            book = next((b for b in BOOKS if b["id"] == int(rest)), None)
            if book:
                self.send_json(200, book_detail(book))
            else:
                self.send_json(404, {"message": "Book not found"})
            return

        if path.startswith(f"{CATALOG_BASE}/files/") and path.endswith("/download"):
            file_id = int(path[len(f"{CATALOG_BASE}/files/"):-len("/download")])
            book_id, fmt_index = divmod(file_id, 10)
            book = next((b for b in BOOKS if b["id"] == book_id), None)
            if book and fmt_index < len(book["formats"]):
                fmt = book["formats"][fmt_index]
                self.send_bytes(200, file_payload(book_id, fmt), "application/octet-stream")
            else:
                self.send_json(404, {"message": "File not found"})
            return

        if path.startswith(f"{CATALOG_BASE}/sections/"):
            name = path[len(f"{CATALOG_BASE}/sections/"):]
            self.send_section(name, query)
            return

        self.send_json(404, {"message": f"no catalog route: {path}"})

    def send_section(self, name: str, query: dict) -> None:
        if name == "libraries":
            items = [{"id": "library:1", "title": "Main", "section": "libraries", "count": len(BOOKS),
                      "booksHref": f"{CATALOG_BASE}/books?libraryId=1&sort=title"}]
        elif name == "collections":
            items = []
        elif name == "smart-scopes":
            items = []
        elif name == "authors":
            authors = sorted({a for b in BOOKS for a in b["authors"]})
            items = [{"id": f"author:{a}", "title": a, "section": "authors",
                      "booksHref": f"{CATALOG_BASE}/books?author={a.replace(' ', '+')}&sort=title"}
                     for a in authors]
        elif name == "series":
            seen = {}
            for b in BOOKS:
                if b["seriesId"] and b["seriesId"] not in seen:
                    seen[b["seriesId"]] = b
            items = [{"id": f"series:{sid}", "title": b["seriesName"], "section": "series", "count": 2,
                      "seriesId": sid,
                      "booksHref": f"{CATALOG_BASE}/books?seriesId={sid}&sort=series"}
                     for sid, b in sorted(seen.items())]
        elif name == "continue-reading":
            self.send_json(200, {"section": name,
                                 "items": [book_summary(b) for b in BOOKS if b["readStatus"] == "reading"]})
            return
        elif name == "recent" or name == "all-books":
            self.send_json(200, books_page({**query, "sort": ["title"] if name == "all-books" else ["recently_added"]}))
            return
        else:
            self.send_json(404, {"message": f"unknown section {name}"})
            return

        self.send_json(200, {"section": name, "items": items})

    def do_PUT(self) -> None:
        path = self.path.split("?")[0]
        length = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(length) if length else b"{}"

        if path == "/api/v1/koreader/syncs/progress":
            if self.check_auth():
                self.log_message("progress PUT: %s", body.decode(errors="replace"))
                self.send_json(200, {"document": "ack", "timestamp": int(time.time())})
            return

        if path.startswith(f"{CATALOG_BASE}/books/"):
            rest = path[len(f"{CATALOG_BASE}/books/"):]
            if rest.endswith("/read-status"):
                if not self.check_auth():
                    return
                book_id = int(rest[: -len("/read-status")])
                status = json.loads(body).get("status")
                for book in BOOKS:
                    if book["id"] == book_id:
                        book["readStatus"] = status
                        self.log_message("book %d read-status -> %s", book_id, status)
                        self.send_json(200, {"readStatus": status})
                        return
                self.send_json(404, {"message": "Book not found"})
                return
            if rest.endswith("/rating"):
                if not self.check_auth():
                    return
                book_id = int(rest[: -len("/rating")])
                rating = json.loads(body).get("rating")
                for book in BOOKS:
                    if book["id"] == book_id:
                        book["rating"] = rating
                        self.log_message("book %d rating -> %s", book_id, rating)
                        self.send_json(200, {"rating": rating})
                        return
                self.send_json(404, {"message": "Book not found"})
                return

        self.send_json(404, {"message": f"no route: {path}"})


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8080)
    args = parser.parse_args()

    server = ThreadingHTTPServer((args.host, args.port), Handler)
    print(f"BookOrbit mock catalog listening on http://{args.host}:{args.port}", flush=True)
    print("Device: Settings -> System -> KOReader Sync -> Sync Server URL = "
          f"http://<this-host>:{args.port}", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()

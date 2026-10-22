#!/usr/bin/env python3
"""Drive the reader with the serial test API (docs/serial-test-api.md), over USB serial or a
native build run as a child process.

    tools/devctl.py [--port P] [--log FILE] [--rotate DEG] STEP...
    tools/devctl.py [--port P] --script steps.txt
    tools/devctl.py --exec PATH [--exec-arg A]... [--cwd DIR] [--log FILE] [--rotate DEG] STEP...

Steps run in order; the run stops at the first failed step and exits non-zero (--keep-going runs
them all and exits non-zero if any failed).

  cmd:<TEXT>            send "CMD:<TEXT>", wait for its reply line, print it
                        (fails on a *_ERROR / *_TIMEOUT reply or none at all)
  state                 CMD:STATE, print the reply and remember its fields for `check:`
  check:<key>=<regex>   the last STATE's <key> must match <regex> (re.fullmatch)
  idle[:<ms>[:SCREEN]]  CMD:IDLE..., wait for IDLE / IDLE_TIMEOUT (a timeout fails the step)
  shot:<file.png>       CMD:SCREENSHOT, decode the 1-bit frame, rotate it, save it
  wait:<seconds>        read (and log) the device's output for a while
  expect:<regex>:<sec>  read until an output line matches (re.search), fail otherwise
  select:<regex>[:<n>]  press Down, waiting for each, until STATE's label matches (re.fullmatch); at most n
                        presses (default 40). The label is checked first, so a row already selected takes none.
  tab:<name>[:<n>]      switch tabs (a long Down) until STATE's tab matches: <name> is the tab's label
                        (re.fullmatch) or, when it is a number, its tab_slot; at most n switches
                        (default 8). The tab is checked first, so the current one takes none.
  set:<label>=<value>  make the row whose label matches <label> (re.fullmatch) show the value
                        <value> (re.fullmatch): confirm the row, and when it opens a selector,
                        pick the option that matches and confirm it. A row that already shows the
                        value takes no press.
  diff:<a.png>:<b.png>[:<n>]
                        the two screenshots must differ in at least n pixels (default 1): "the screen
                        changed" as an assertion

A --script file holds one step per line; blank lines and lines starting with # are skipped.
Every line the device prints goes to --log with a timestamp. Needs pyserial for --port alone: the
native flow and the screenshots need nothing but the standard library.

The port is opened with the default DTR/RTS lines, which does not reset an ESP32-C3 (unlike
tools/serial_transfer.py, which deasserts them), and closed with RTS dropped before DTR, which does
not either (a plain close on Windows does). The device re-enumerates its USB port across a
reboot or a sleep; the reader reopens it and carries on.

--exec runs a native build of the firmware as a child process instead of talking to a port. The
child speaks the same protocol on its stdin/stdout (a command line in, the same reply lines and
the same binary screenshot frame out) and writes its log to stderr, which is drained on its own
thread into --log with the same timestamps. The commands, the checks and the screenshots work the
same way; only the transport differs. The child's stdout is read as raw bytes: the screenshot
frame is never decoded as text. The child's stdin stays open while the steps run and is closed
at the end, which is how the child is told to finish. pyserial is needed for --port alone, so a
native run needs no pyserial.
"""
import argparse
import os
import re
import select
import struct
import subprocess
import sys
import threading
import time
import zlib

# First words of every reply the firmware sends to a command (see docs/serial-test-api.md).
REPLY = re.compile(
    r"^(BTN_|ACT_|STATE:|CMD_ERROR|GET:|GET_END|GET_ERROR|SET_|LOG_|TOUCH_|SWIPE_|SLEEP_OK|REBOOT_OK|"
    r"EVENTS_|IDLE|DICT|HELP:|BOOTLOG_|HEAP_|TTF:|TTF_|FS:|FS_)"
)
# Commands that answer with several lines, and the line that ends them.
MULTI_LINE = {"GET:*": re.compile(r"^GET_END"), "FS:LS:": re.compile(r"^FS_END")}
FAILED = re.compile(r"_ERROR|_TIMEOUT|^CMD_ERROR")


def is_text(data):
    """True if the raw bytes look like a text line, and not a piece of the binary frame.

    Bytes over 127 are allowed: the device prints UTF-8 (book titles). Only the control
    characters a text line does not hold (tab, newline, carriage return, escape) are text.
    """
    text_controls = (9, 10, 13, 27)
    return not any(byte < 32 and byte not in text_controls for byte in data)


# --- PNG, with nothing but the standard library ----------------------------------------------
# The device sends a 1-bit frame: rows from top to bottom, 8 pixels per byte, the leftmost pixel
# in the highest bit, a set bit is white. These helpers turn that into a PNG the host can open,
# and read one back, so the whole flow needs no third-party package.

def frame_rows(width, height, data, rotate):
    """The frame as rows of 0 (black) and 255 (white) bytes, turned as asked.

    `rotate` is a PIL-style angle: -90 is a quarter turn clockwise, 90 counter-clockwise, 180
    upside down, 0 keeps the panel's own frame.
    """
    stride = (width + 7) // 8
    rows = []
    for y in range(height):
        row = bytearray(width)
        base = y * stride
        for x in range(width):
            row[x] = 255 if (data[base + (x >> 3)] >> (7 - (x & 7))) & 1 else 0
        rows.append(row)
    if rotate == 0:
        return rows, width, height
    if rotate == 180:
        return [row[::-1] for row in rows[::-1]], width, height
    turned = [bytearray(height) for _ in range(width)]
    if rotate == -90:  # a quarter turn clockwise
        for y, source in enumerate(rows):
            for x, pixel in enumerate(source):
                turned[x][height - 1 - y] = pixel
    elif rotate == 90:  # a quarter turn counter-clockwise
        for y, source in enumerate(rows):
            for x, pixel in enumerate(source):
                turned[width - 1 - x][y] = pixel
    else:
        raise ValueError(f"unsupported rotation {rotate}")
    return turned, height, width


def png_chunk(tag, body):
    return (struct.pack(">I", len(body)) + tag + body +
            struct.pack(">I", zlib.crc32(tag + body) & 0xFFFFFFFF))


def write_png(path, width, height, rows):
    """Write 8-bit greyscale rows as a PNG."""
    raw = b"".join(b"\x00" + bytes(row) for row in rows)
    header = struct.pack(">IIBBBBB", width, height, 8, 0, 0, 0, 0)
    png = (b"\x89PNG\r\n\x1a\n" + png_chunk(b"IHDR", header) +
           png_chunk(b"IDAT", zlib.compress(raw, 9)) + png_chunk(b"IEND", b""))
    with open(path, "wb") as handle:
        handle.write(png)


def read_png_gray(path):
    """The rows of a PNG written by write_png(): (width, height, rows).

    Only the shape this module writes is read: 8-bit greyscale, no interlacing, every row
    stored unfiltered. Another tool's PNG raises ValueError.
    """
    with open(path, "rb") as handle:
        data = handle.read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG")
    pos = 8
    width = height = 0
    compressed = b""
    while pos + 8 <= len(data):
        length = struct.unpack(">I", data[pos:pos + 4])[0]
        tag = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if tag == b"IHDR":
            width, height, depth, colour, _, _, interlace = struct.unpack(">IIBBBBB", body)
            if depth != 8 or colour != 0 or interlace:
                raise ValueError("not an 8-bit greyscale PNG")
        elif tag == b"IDAT":
            compressed += body
        elif tag == b"IEND":
            break
    raw = zlib.decompress(compressed)
    rows = []
    for y in range(height):
        start = y * (width + 1)
        if raw[start] != 0:
            raise ValueError("unsupported PNG row filter")
        rows.append(bytearray(raw[start + 1:start + 1 + width]))
    return width, height, rows


def native_path(path):
    """A Git Bash path (/c/tmp/x.png) as Windows' Python opens it (C:/tmp/x.png); others as they are."""
    match = re.match(r"^/([a-zA-Z])/(.*)$", path)
    return f"{match.group(1).upper()}:/{match.group(2)}" if os.name == "nt" and match else path


def split_fields(text):
    """key=value pairs of a STATE line; values in double quotes may hold spaces (\\ escapes)."""
    fields = {}
    i, n = 0, len(text)
    while i < n:
        while i < n and text[i] == " ":
            i += 1
        if i >= n:
            break
        eq = text.find("=", i)
        if eq < 0:
            break
        key = text[i:eq]
        i = eq + 1
        value = []
        if i < n and text[i] == '"':
            i += 1
            while i < n and text[i] != '"':
                if text[i] == "\\" and i + 1 < n:
                    i += 1
                value.append(text[i])
                i += 1
            i += 1
        else:
            while i < n and text[i] != " ":
                value.append(text[i])
                i += 1
        fields[key] = "".join(value)
    return fields


class LogTail:
    """The device's log lines, kept so that `expect` can match them.

    In --exec mode the log comes on the child's stderr, which is drained on its own thread: the
    log never mixes into the protocol stream, so matching a log line cannot touch the binary
    screenshot frame. A line is matched once, so a later `expect` does not match it again.
    """

    def __init__(self, limit=5000):
        self.limit = limit
        self.lines = []  # (number, text)
        self.next_number = 0
        self.matched = set()  # the numbers an `expect` has already matched
        self.lock = threading.Lock()

    def append(self, line):
        with self.lock:
            self.lines.append((self.next_number, line))
            self.next_number += 1
            if len(self.lines) > self.limit:
                for number, _ in self.lines[:len(self.lines) - self.limit]:
                    self.matched.discard(number)
                del self.lines[:len(self.lines) - self.limit]

    def mark(self):
        """The number the next line will get: the lines before it are older than the last
        command that was sent."""
        with self.lock:
            return self.next_number

    def take(self, pattern, since=0):
        """The oldest line that is not older than `since`, is not matched yet, and matches
        `pattern`; or None."""
        with self.lock:
            for number, text in self.lines:
                if number < since or number in self.matched:
                    continue
                if pattern.search(text):
                    self.matched.add(number)
                    return text
        return None


def pyserial():
    """pyserial, imported only when a port is used."""
    try:
        import serial
    except ImportError:
        sys.exit("pyserial is required for --port: pip install pyserial")
    return serial


class SerialTransport:
    """The device's USB serial port."""

    def __init__(self, port):
        self.port = port
        self.serial = pyserial()
        self.ser = self._open()

    def _open(self):
        for _ in range(60):
            try:
                s = self.serial.Serial(self.port, 115200, timeout=0.2)
                s.write(b"\r\n")  # C3 USB-CDC output waits until the host has written something
                return s
            except (self.serial.SerialException, OSError):
                time.sleep(0.5)
        sys.exit(f"cannot open {self.port}")

    def read(self, size, timeout=0.2):
        try:
            return self.ser.read(size)
        except (self.serial.SerialException, OSError):
            # The port went away (reboot, sleep, re-enumeration): reopen and go on.
            try:
                self.close()
            except Exception:
                pass
            time.sleep(1)
            self.ser = self._open()
            return b""

    def write(self, data):
        self.ser.write(data)

    def pending(self):
        try:
            return self.ser.in_waiting
        except Exception:
            return 0

    def close(self):
        # RTS goes low before DTR: a plain close on Windows drops DTR first, and the ESP32-C3's USB serial unit
        # takes RTS high with DTR low as a reset request (esptool's), so every run restarted the device to Home.
        if self.ser.is_open:
            self.ser.rts = False
            self.ser.dtr = False
        self.ser.close()

    @property
    def dead(self):
        """A serial port never ends by itself: the device reboots and comes back."""
        return False


class ExecTransport:
    """A native build of the firmware, run as a child process.

    Commands go to its stdin, the protocol (reply lines and the binary screenshot frame) comes
    back on its stdout, and its log comes on its stderr. The stderr is drained on its own
    thread into the log file, so a chatty log cannot stall the protocol and the log never
    mixes into the binary frame. The stdout is read as raw bytes and never decoded as text
    before the frame is read exactly.
    """

    def __init__(self, path, args=(), cwd=None, on_log=None):
        self.on_log = on_log
        try:
            self.proc = subprocess.Popen([path, *args], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                         stderr=subprocess.PIPE, cwd=cwd, bufsize=0)
        except OSError as error:
            sys.exit(f"cannot run {path}: {error}")
        self.eof = False
        self.pump = threading.Thread(target=self._drain_stderr, daemon=True)
        self.pump.start()

    def _drain_stderr(self):
        fd = self.proc.stderr.fileno()
        buf = b""
        while True:
            try:
                data = os.read(fd, 4096)
            except OSError:
                break
            if not data:
                break
            buf += data
            while (nl := buf.find(b"\n")) >= 0:
                line, buf = buf[:nl], buf[nl + 1:]
                self._log(line.decode("utf-8", errors="replace").rstrip("\r"))
        if buf:
            self._log(buf.decode("utf-8", errors="replace").rstrip("\r"))

    def _log(self, line):
        if self.on_log:
            self.on_log(line)

    def read(self, size, timeout=0.2):
        """Raw bytes from the child's stdout; b"" when nothing came in the timeout or at the end."""
        if self.eof:
            return b""
        fd = self.proc.stdout.fileno()
        try:
            ready, _, _ = select.select([fd], [], [], timeout)
        except (OSError, ValueError):
            self.eof = True
            return b""
        if not ready:
            return b""
        try:
            data = os.read(fd, size)
        except OSError:
            self.eof = True
            return b""
        if not data:
            self.eof = True
        return data

    def write(self, data):
        try:
            fd = self.proc.stdin.fileno()
            while data:
                written = os.write(fd, data)
                data = data[written:]
        except (BrokenPipeError, OSError):
            self.eof = True

    def pending(self):
        """Bytes waiting on stdout, without consuming them."""
        try:
            import fcntl
            import struct
            import termios
            buf = fcntl.ioctl(self.proc.stdout.fileno(), termios.FIONREAD, b"\0" * 4)
            return struct.unpack("i", buf)[0]
        except Exception:
            pass
        # No FIONREAD: say there is something there if the pipe is readable, and let the read
        # take what is there.
        try:
            ready, _, _ = select.select([self.proc.stdout.fileno()], [], [], 0)
            return 65536 if ready else 0
        except (OSError, ValueError):
            return 0

    @property
    def dead(self):
        """True when no more protocol bytes can arrive: the child is gone, or its stdout is
        closed and drained. A command that waits for a reply gives up at once instead of
        sitting out its whole timeout."""
        return self.eof or self.proc.poll() is not None

    def shutdown(self, timeout=15.0):
        """Close the child's stdin (its end of file, which is how it is told to finish), wait for
        it to go, and insist if it does not. Returns the exit code and whether it had to be
        stopped the hard way."""
        try:
            self.proc.stdin.close()
        except OSError:
            pass
        try:
            code = self.proc.wait(timeout=timeout)
            forced = False
        except subprocess.TimeoutExpired:
            self.proc.terminate()  # the child handles SIGTERM and winds up on its own
            try:
                code = self.proc.wait(timeout=5.0)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                code = self.proc.wait()
            forced = True
        # Whatever the child left on stdout before it went: keep it for the log.
        self.trailing = self._drain_stdout()
        self.pump.join(timeout=2.0)
        return code, forced

    def _drain_stdout(self):
        fd = self.proc.stdout.fileno()
        out = b""
        while True:
            try:
                ready, _, _ = select.select([fd], [], [], 0.05)
            except (OSError, ValueError):
                break
            if not ready:
                break
            try:
                data = os.read(fd, 65536)
            except OSError:
                break
            if not data:
                break
            out += data
        return out

    def close(self):
        return self.shutdown()


class Device:
    """The device, over a transport: the USB serial port, or a native build run as a child.

    Commands, replies and the binary screenshot frame are the same either way; only where the
    bytes come from differs. The device's log (the child's stderr, in --exec mode) is kept
    apart from the protocol stream, so `expect` can match it without touching the frame.
    """

    def __init__(self, transport, log_path=None):
        self.transport = transport
        self.log = open(log_path, "a", encoding="utf-8", errors="replace") if log_path else None
        self.log_lock = threading.Lock()
        self.t0 = time.time()
        self.buf = b""
        self.tail = LogTail()
        self.log_mark = 0  # the log lines older than this are not matched by `expect`
        self.deadline = None  # Optional monotonic host-session deadline; scripted mode is unbounded.

    @classmethod
    def serial(cls, port, log_path=None):
        return cls(SerialTransport(port), log_path)

    @classmethod
    def child(cls, path, args=(), cwd=None, log_path=None):
        device = cls(None, log_path)
        device.transport = ExecTransport(path, args, cwd, device.on_log_line)
        return device

    def note(self, line):
        with self.log_lock:
            if self.log:
                self.log.write(f"[{time.time() - self.t0:8.2f}] {line}\n")
                self.log.flush()

    def on_log_line(self, line):
        """A line from the device's log (the child's stderr): into the log file and into the
        tail that `expect` matches."""
        self.tail.append(line)
        self.note(line)

    def close(self):
        """Close the transport. For a child: close its stdin (its end of file), wait for it to
        finish, and log whatever it left on stdout."""
        result = self.transport.close()
        trailing = getattr(self.transport, "trailing", b"")
        for raw in trailing.split(b"\n"):
            if raw and is_text(raw):
                self.note(raw.decode("utf-8", errors="replace").rstrip("\r"))
        with self.log_lock:
            if self.log:
                self.log.close()
                self.log = None
        return result

    def _read(self, size):
        return self.transport.read(size)

    def read_line(self, deadline):
        """The next protocol line, or None when the deadline passes or the device is gone.

        Only text is decoded. A piece of the binary frame is never taken for a line: it is
        noted and the read gives up, so a frame that arrives out of band fails loudly instead
        of being decoded as text.
        """
        if self.deadline is not None:
            deadline = min(deadline, time.time() + max(0, self.deadline - time.monotonic()))
        while time.time() < deadline:
            nl = self.buf.find(b"\n")
            if nl >= 0:
                raw, self.buf = self.buf[:nl], self.buf[nl + 1:]
                if not is_text(raw):
                    self.note(f"<binary: {len(raw)} bytes>")
                    return None
                text = raw.decode("utf-8", errors="replace").rstrip("\r")
                self.note(text)
                return text
            data = self._read(4096)
            if not data:
                if self.transport.dead:
                    return None  # the child is gone: no reply is coming
                time.sleep(0.01)
                continue
            self.buf += data
        return None

    def read_exact(self, size, deadline):
        """Exactly `size` raw bytes, or fewer if the deadline passes or the device is gone.
        The bytes are never decoded: this is where the screenshot frame comes from."""
        if self.deadline is not None:
            deadline = min(deadline, time.time() + max(0, self.deadline - time.monotonic()))
        while len(self.buf) < size and time.time() < deadline:
            data = self._read(min(65536, size - len(self.buf)))
            if not data:
                if self.transport.dead:
                    break
                time.sleep(0.01)
                continue
            self.buf += data
        data, self.buf = self.buf[:size], self.buf[size:]
        return data

    def drain(self):
        """Log and drop the lines already received, so a late reply to an earlier command (one that
        timed out here) cannot pass for the reply to the next. A partial line is kept."""
        while True:
            waiting = self.transport.pending()
            if not waiting:
                break
            data = self._read(waiting)
            if not data:
                break
            self.buf += data
        while (nl := self.buf.find(b"\n")) >= 0:
            raw, self.buf = self.buf[:nl], self.buf[nl + 1:]
            if is_text(raw):
                self.note(raw.decode("utf-8", errors="replace").rstrip("\r"))

    def send(self, text):
        if self.deadline is not None and time.monotonic() >= self.deadline:
            raise TimeoutError("host session deadline exceeded")
        self.drain()
        self.note(f">>> CMD:{text}")
        # The log lines from now on belong to this command: `expect` after it matches those.
        self.log_mark = self.tail.mark()
        self.transport.write(("CMD:" + text + "\n").encode())

    def command(self, text, timeout=20, until=None, lines=None):
        """Send a command; return its reply line (the terminator of a several-line reply), or None. With `until`,
        the lines before the terminator that are not log lines go to `lines`."""
        self.send(text)
        deadline = time.time() + timeout
        while (line := self.read_line(deadline)) is not None:
            if until is not None:
                if until.match(line) or FAILED.search(line):
                    return line
                if lines is not None and REPLY.match(line):
                    lines.append(line)
                continue
            if REPLY.match(line):
                return line
        return None

    def expect_line(self, pattern, deadline):
        """Wait for a line matching `pattern`: the device's log (the child's stderr, in --exec
        mode) or a line on the protocol stream. The log is matched first, so a log line is never
        mistaken for a reply and the binary frame is left alone."""
        while True:
            line = self.tail.take(pattern, self.log_mark)
            if line is not None:
                return line
            if time.time() >= deadline:
                return None
            line = self.read_line(min(deadline, time.time() + 0.2))
            if line is not None and pattern.search(line):
                return line

    def screenshot(self, path, rotate):
        self.send("SCREENSHOT")
        deadline = time.time() + 30
        while (line := self.read_line(deadline)) is not None:
            if line.startswith("SCREENSHOT_START:"):
                _, w, h, size = line.split(":")
                data = self.read_exact(int(size), time.time() + 30)
                if len(data) != int(size):
                    return f"short frame: {len(data)} of {size} bytes"
                rows, width, height = frame_rows(int(w), int(h), data, rotate)
                write_png(path, width, height, rows)
                end = self.read_line(time.time() + 5)
                return None if end == "SCREENSHOT_END" else f"no SCREENSHOT_END (got {end!r})"
            if line.startswith("SCREENSHOT_ERROR"):
                return line
        return "no reply"


def idle_timeout(arg):
    """Seconds to wait for the reply to CMD:IDLE<arg>: the device's own timeout and some slack."""
    first = arg.split(":")[0] if arg else ""
    return (int(first) if first.isdigit() else 10000) / 1000 + 5


def read_state(device):
    """The device's STATE fields, as a dict (empty when it does not answer)."""
    reply = device.command("STATE")
    if not reply or not reply.startswith("STATE:"):
        return {}
    return split_fields(reply[len("STATE:"):])


def set_setting(device, label, wanted, state):
    """Make the row whose label matches `label` show the value `wanted`.

    A setting row is one of two kinds: one that opens a selector (a list of choices to pick
    from) and one that cycles through its choices in place. Confirming the row does the right
    thing for either, but a script cannot know which it is, so this looks: confirm the row and,
    when a selector came up, pick the wanted option in it and confirm that. The label is matched
    as a regular expression against the whole string; the value is matched the same way but
    without regard to case, because the device writes "ON" where a script would say "on".
    """
    row = read_state(device)
    for press in range(41):
        if re.fullmatch(label, row.get("label", "")):
            break
        if press == 40:
            print(f"ROW NOT FOUND: {label} (last label {row.get('label')!r})")
            return False
        device.command("BTN:DOWN")
        device.command("IDLE:5000", timeout=idle_timeout("5000"))
        row = read_state(device)
    for attempt in range(5):
        if re.fullmatch(wanted, row.get("value", ""), re.IGNORECASE):
            print(f"{row.get('label')!r} is {row.get('value')!r}")
            state.update(row)
            return True
        before = row.get("activity", "")
        device.command("BTN:CONFIRM")
        device.command("IDLE:5000", timeout=idle_timeout("5000"))
        after = read_state(device)
        if after.get("activity", "") != before:
            # A selector came up: pick the wanted option in it.
            for press in range(60):
                if re.fullmatch(wanted, after.get("label", ""), re.IGNORECASE):
                    break
                if press == 59:
                    print(f"OPTION NOT FOUND: {wanted} (last option {after.get('label')!r})")
                    return False
                device.command("BTN:DOWN")
                device.command("IDLE:5000", timeout=idle_timeout("5000"))
                after = read_state(device)
            device.command("BTN:CONFIRM")
            device.command("IDLE:5000", timeout=idle_timeout("5000"))
        row = read_state(device)
    print(f"VALUE NOT REACHED: {label} -> {wanted} (now {row.get('value')!r})")
    return False


def run(device, steps, rotate, keep_going=False):
    state = {}
    all_ok = True
    for step in steps:
        kind, _, rest = step.partition(":")
        ok = True
        if kind == "cmd":
            until = next((end for start, end in MULTI_LINE.items() if rest.startswith(start)), None)
            timeout = idle_timeout(rest[5:]) if rest.startswith("IDLE") else 20
            lines = []
            reply = device.command(rest, timeout=timeout, until=until, lines=lines)
            for line in lines:
                print(line)
            print(reply if reply else f"NO REPLY to {rest}")
            ok = bool(reply) and not FAILED.search(reply)
        elif kind == "state":
            reply = device.command("STATE")
            print(reply if reply else "NO REPLY to STATE")
            ok = bool(reply) and reply.startswith("STATE:")
            if ok:
                state = split_fields(reply[len("STATE:"):])
        elif kind == "check":
            key, _, pattern = rest.partition("=")
            value = state.get(key)
            ok = value is not None and re.fullmatch(pattern, value) is not None
            print(f"check {key}={value!r} ~ /{pattern}/: {'ok' if ok else 'FAILED'}")
        elif kind == "idle":
            reply = device.command("IDLE" + (":" + rest if rest else ""), timeout=idle_timeout(rest))
            print(reply if reply else "NO REPLY to IDLE")
            ok = bool(reply) and reply.startswith("IDLE:")
        elif kind == "shot":
            path = native_path(rest)
            error = device.screenshot(path, rotate)
            print(f"screenshot -> {path}" if error is None else f"SCREENSHOT FAILED: {error}")
            ok = error is None
        elif kind == "select":
            pattern, _, most = rest.partition(":")
            presses = int(most) if most.isdigit() else 40
            ok = False
            for press in range(presses + 1):
                reply = device.command("STATE")
                fields = split_fields(reply[len("STATE:"):]) if reply and reply.startswith("STATE:") else {}
                if re.fullmatch(pattern, fields.get("label", "")):
                    state = fields
                    ok = True
                    print(f"selected {fields.get('label')!r} after {press} presses")
                    break
                if press == presses:
                    break
                # One press at a time: presses sent back to back reach the list faster than it redraws, and it
                # moves further than asked.
                device.command("BTN:DOWN")
                device.command("IDLE:5000", timeout=idle_timeout("5000"))
            if not ok:
                print(f"NOT SELECTED: {pattern} (last label {fields.get('label')!r})")
        elif kind == "set":
            label, _, wanted = rest.partition("=")
            ok = set_setting(device, label, wanted, state)
        elif kind == "tab":
            pattern, _, most = rest.partition(":")
            presses = int(most) if most.isdigit() else 8
            ok = False
            fields = {}
            for press in range(presses + 1):
                reply = device.command("STATE")
                fields = split_fields(reply[len("STATE:"):]) if reply and reply.startswith("STATE:") else {}
                if pattern.isdigit():
                    matched = fields.get("tab_slot") == pattern
                else:
                    matched = re.fullmatch(pattern, fields.get("tab", "")) is not None
                if matched:
                    state = fields
                    ok = True
                    print(f"on tab {fields.get('tab')!r} (slot {fields.get('tab_slot')}) after {press} switches")
                    break
                if press == presses:
                    break
                # A long Down steps to the next tab; the list answers on the next pass.
                device.command("BTN:DOWN:LONG")
                device.command("IDLE:5000", timeout=idle_timeout("5000"))
            if not ok:
                print(f"TAB NOT REACHED: {pattern} (last tab {fields.get('tab')!r})")
        elif kind == "diff":
            first, _, tail = rest.partition(":")
            second, _, most = tail.partition(":")
            minimum = int(most) if most.isdigit() else 1
            try:
                left_width, left_height, left_rows = read_png_gray(native_path(first))
                right_width, right_height, right_rows = read_png_gray(native_path(second))
            except (OSError, ValueError) as error:
                print(f"diff {first} {second}: cannot read the images: {error}")
                ok = False
            else:
                if (left_width, left_height) != (right_width, right_height):
                    print(f"diff {first} {second}: different sizes "
                          f"{left_width}x{left_height} vs {right_width}x{right_height}")
                    ok = False
                else:
                    changed = sum(sum(1 for x, y in zip(a, b) if x != y) for a, b in zip(left_rows, right_rows))
                    ok = changed >= minimum
                    print(f"diff {first} {second}: {changed} pixels differ (need at least {minimum})")
        elif kind == "wait":
            end = time.time() + float(rest)
            while device.read_line(end) is not None:
                pass
        elif kind == "expect":
            pattern, _, seconds = rest.rpartition(":")
            end = time.time() + float(seconds)
            found = device.expect_line(re.compile(pattern), end)
            print(f"matched: {found}" if found else f"NOT SEEN: {pattern}")
            ok = found is not None
        else:
            print(f"unknown step {step!r}")
            ok = False
        if not ok:
            print(f"step failed: {step}")
            all_ok = False
            if not keep_going:
                return False
    return all_ok


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    where = ap.add_mutually_exclusive_group()
    where.add_argument("--port", default="/dev/ttyACM0", help="the device's serial port")
    where.add_argument("--exec", dest="exec_path", default=None,
                       help="run this native build of the firmware as a child process and talk to it "
                            "over its stdin/stdout (its log comes on stderr)")
    ap.add_argument("--exec-arg", dest="exec_args", action="append", default=[],
                    help="an argument for the --exec program (repeatable)")
    ap.add_argument("--cwd", default=None, help="working directory for the --exec program")
    ap.add_argument("--log", default=None, help="append the device's output here, timestamped")
    ap.add_argument("--rotate", type=int, default=-90,
                    help="rotate screenshots by this many degrees (default -90: the X4's 800x480 frame "
                         "upright in portrait; 0 keeps the panel's native frame)")
    ap.add_argument("--script", default=None, help="read steps from this file, one per line")
    ap.add_argument("--keep-going", action="store_true", help="run every step even after one failed")
    ap.add_argument("steps", nargs="*")
    args = ap.parse_args()

    steps = list(args.steps)
    if args.script:
        with open(args.script, encoding="utf-8") as f:
            steps += [s.strip() for s in f if s.strip() and not s.lstrip().startswith("#")]
    if not steps:
        ap.error("no steps given")

    # Labels, chapter titles and popups may be in any script: a console's code page must not stop the run.
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    log_path = native_path(args.log) if args.log else None
    if args.exec_path:
        device = Device.child(args.exec_path, args.exec_args, args.cwd, log_path)
    else:
        device = Device.serial(args.port, log_path)
    try:
        ok = run(device, steps, args.rotate, args.keep_going)
    finally:
        # Whatever went wrong, the port closes RTS before DTR: an exception that closed it plainly would reset the
        # device (see close()). A child gets its stdin closed (its end of file, which is how it is told to finish)
        # and is waited for.
        result = device.close()
    if args.exec_path and result:
        code, forced = result
        print(f"child exited with code {code}" + (" (it had to be stopped)" if forced else ""))
        if forced or code != 0:
            ok = False
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()

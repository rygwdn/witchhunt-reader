#!/usr/bin/env python3
"""Persistent JSONL reader controller. Uses devctl's sole transport owner.

python3 tools/devctl_interactive.py --exec .pio/build/simulator/program --output-dir /tmp/session
Requests: {"op":"observe"}, {"op":"press","button":"DOWN","kind":"long"}, {"op":"close"}.
Only JSON replies go to stdout. Screenshot paths refer to local PNG files.
"""
import argparse
import json
from pathlib import Path
import re
import sys

from devctl import Device, idle_timeout, split_fields

BUTTONS = ('BACK', 'CONFIRM', 'LEFT', 'RIGHT', 'UP', 'DOWN', 'POWER')
KINDS = {'short': '', 'long': ':LONG', 'double': ':DOUBLE'}


class InteractiveSession:
    def __init__(self, device, output_dir, rotate=-90, idle_ms=10000):
        self.device = device
        self.directory = Path(output_dir).resolve()
        self.directory.mkdir(parents=True, exist_ok=True)
        self.rotate = rotate
        self.idle_ms = idle_ms
        self.sequence = 0
        self.closed = False
        self.fatal = False
        self.help = device.command('HELP')
        capabilities = set(re.findall(r'[A-Z_]+', self.help or ''))
        self.missing = sorted({'BTN', 'STATE', 'SCREENSHOT', 'IDLE'} - capabilities)
        self.actions = [button + suffix for button in BUTTONS for suffix in KINDS.values()]

    def observe(self):
        observation = {'state': {}, 'screenshot': None, 'idle': None, 'errors': [],
                       'actions': self.actions, 'usable': False}
        if self.closed or self.fatal or self.missing:
            observation['errors'].append('session closed or framing lost' if self.closed or self.fatal
                                         else 'missing capabilities: ' + ','.join(self.missing))
            return observation
        arg = f'{self.idle_ms}:SCREEN'
        observation['idle'] = self.device.command('IDLE:' + arg, timeout=idle_timeout(arg))
        if not (observation['idle'] or '').startswith('IDLE:'):
            observation['errors'].append('screen did not settle')
        reply = self.device.command('STATE')
        if reply and reply.startswith('STATE:'):
            observation['state'] = split_fields(reply[6:])
        else:
            observation['errors'].append('STATE unavailable')
        self.sequence += 1
        path = self.directory / f'frame-{self.sequence:04d}.png'
        error = self.device.screenshot(str(path), self.rotate)
        if error:
            observation['errors'].append(error)
            # A framed explicit error is safe; every other capture failure may leave binary bytes.
            if not error.startswith('SCREENSHOT_ERROR'):
                self.fatal = True
        else:
            observation['screenshot'] = str(path)
        observation['usable'] = not observation['errors'] and bool(observation['state'])
        return observation

    def press(self, button, kind='short'):
        if button not in BUTTONS or kind not in KINDS:
            raise ValueError('unsupported button or press kind')
        if self.closed or self.fatal or self.missing:
            return self.observe()
        reply = self.device.command('BTN:' + button + KINDS[kind])
        observation = self.observe()  # Never retry an uncertain press.
        observation['press_reply'] = reply
        if reply != 'BTN_OK':
            observation['errors'].append('press not acknowledged; effect uncertain')
            observation['usable'] = False
        return observation

    def close(self):
        if self.closed:
            return None
        self.closed = True
        return self.device.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    transport = parser.add_mutually_exclusive_group(required=True)
    transport.add_argument('--exec', dest='executable')
    transport.add_argument('--port')
    parser.add_argument('--output-dir', required=True)
    parser.add_argument('--rotate', type=int, choices=(-90, 0, 90, 180), default=-90)
    args = parser.parse_args()
    device = Device.child(args.executable) if args.executable else Device.serial(args.port)
    session = None
    failed = False
    try:
        session = InteractiveSession(device, args.output_dir, args.rotate)
        for line in sys.stdin:
            closing = False
            try:
                request = json.loads(line)
                if not isinstance(request, dict):
                    raise ValueError('request must be an object')
                op = request.get('op')
                if op == 'observe':
                    result = session.observe()
                elif op == 'press':
                    result = session.press(request.get('button'), request.get('kind', 'short'))
                elif op == 'close':
                    shutdown = session.close()
                    failed = bool(shutdown and (shutdown[0] != 0 or shutdown[1]))
                    result = {'closed': True, 'shutdown': shutdown, 'ok': not failed}
                    closing = True
                else:
                    raise ValueError('op must be observe, press or close')
            except (ValueError, TypeError) as error:
                result = {'error': str(error)}
            print(json.dumps(result), flush=True)
            if closing:
                break
    finally:
        shutdown = session.close() if session else device.close()
        failed = failed or bool(shutdown and (shutdown[0] != 0 or shutdown[1]))
    return int(failed)


if __name__ == '__main__':
    sys.exit(main())

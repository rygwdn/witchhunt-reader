"""Behavioral regressions for bounded host navigation. Run: python3 -m unittest discover -s tools -p 'test_devctl_*.py'."""
import tempfile
from pathlib import Path
import time
import unittest

from devctl import Device, write_png
from devctl_goal import fingerprint


class ProgressTests(unittest.TestCase):
    def test_volatile_telemetry_does_not_hide_repeated_screen(self):
        with tempfile.TemporaryDirectory() as directory:
            first = Path(directory) / 'first.png'
            second = Path(directory) / 'second.png'
            rows = [bytearray([255, 0]), bytearray([0, 255])]
            write_png(str(first), 2, 2, rows)
            write_png(str(second), 2, 2, rows)
            before = {'state': {'activity': 'Settings', 'label': 'Language', 'heap': '60000', 'popup_ms': '100'}, 'screenshot': str(first)}
            after = {'state': {'activity': 'Settings', 'label': 'Language', 'heap': '59900', 'popup_ms': '900'}, 'screenshot': str(second)}
            self.assertEqual(fingerprint(before), fingerprint(after))
            after['state']['label'] = 'Font size'
            self.assertNotEqual(fingerprint(before), fingerprint(after))

    def test_page_turn_and_pixel_change_both_count_as_progress(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'frame.png'
            write_png(str(path), 2, 2, [bytearray([255, 0]), bytearray([0, 255])])
            observation = {'state': {'activity': 'EpubReader', 'page': '1/20'}, 'screenshot': str(path)}
            initial = fingerprint(observation)
            observation['state']['page'] = '2/20'
            self.assertNotEqual(initial, fingerprint(observation))
            observation['state']['page'] = '1/20'
            write_png(str(path), 2, 2, [bytearray([0, 0]), bytearray([0, 255])])
            self.assertNotEqual(initial, fingerprint(observation))


class DeadlineTests(unittest.TestCase):
    def test_expired_deadline_prevents_writing_another_button(self):
        class Transport:
            writes = []
            def pending(self):
                return 0
            def write(self, data):
                self.writes.append(data)
        transport = Transport()
        device = Device(transport)
        device.deadline = time.monotonic() - 1
        with self.assertRaises(TimeoutError):
            device.command('BTN:CONFIRM')
        self.assertEqual(transport.writes, [])


if __name__ == '__main__':
    unittest.main()

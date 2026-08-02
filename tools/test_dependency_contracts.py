#!/usr/bin/env python3
from pathlib import Path
import unittest
ROOT = Path(__file__).resolve().parents[1]
class DependencyContracts(unittest.TestCase):
    def test_serial_gateway_does_not_depend_on_profile_or_composition(self):
        files = [ROOT/'components/serial_gateway/include/serial_gateway.h',
                 ROOT/'components/serial_gateway/serial_gateway.c',
                 ROOT/'components/serial_gateway/CMakeLists.txt']
        text = '\n'.join(path.read_text() for path in files)
        for forbidden in ('robot_profile', 'robot_composition', 'actuation_coordinator'):
            self.assertNotIn(forbidden, text)
if __name__ == '__main__': unittest.main()

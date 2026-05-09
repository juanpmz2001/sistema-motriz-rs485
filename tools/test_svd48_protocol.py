#!/usr/bin/env python3
"""Host-side protocol checks for UU Motor SVD48 frames."""

from __future__ import annotations

import unittest


def crc16_uumotor(data: bytes) -> int:
    crc = 0xFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            if crc & 0x0001:
                crc = (crc >> 1) ^ 0xA001
            else:
                crc >>= 1
    return crc & 0xFFFF


def read_request(slave: int, reg: int, qty: int) -> bytes:
    frame = bytes([slave, 0x03, reg >> 8, reg & 0xFF, qty >> 8, qty & 0xFF])
    crc = crc16_uumotor(frame)
    return frame + bytes([crc >> 8, crc & 0xFF])


def write_single(slave: int, reg: int, value: int) -> bytes:
    value &= 0xFFFF
    frame = bytes([slave, 0x06, reg >> 8, reg & 0xFF, value >> 8, value & 0xFF])
    crc = crc16_uumotor(frame)
    return frame + bytes([crc >> 8, crc & 0xFF])


class SVD48ProtocolTest(unittest.TestCase):
    def test_manual_read_speed_example(self) -> None:
        self.assertEqual(read_request(0xEE, 0x5410, 2).hex(" ").upper(), "EE 03 54 10 00 02 61 C3")

    def test_rs485_id_1_examples(self) -> None:
        self.assertEqual(read_request(0x01, 0x5410, 2).hex(" ").upper(), "01 03 54 10 00 02 FE D5")
        self.assertEqual(read_request(0x01, 0x5414, 2).hex(" ").upper(), "01 03 54 14 00 02 3F 94")
        self.assertEqual(read_request(0x01, 0x5418, 4).hex(" ").upper(), "01 03 54 18 00 04 3E D4")
        self.assertEqual(read_request(0x01, 0x5420, 4).hex(" ").upper(), "01 03 54 20 00 04 F3 55")

    def test_write_examples(self) -> None:
        self.assertEqual(write_single(0x01, 0x5300, 1).hex(" ").upper(), "01 06 53 00 00 01 4E 59")
        self.assertEqual(write_single(0x01, 0x5304, 100).hex(" ").upper(), "01 06 53 04 00 64 A4 D8")
        self.assertEqual(write_single(0x01, 0x5304, -100).hex(" ").upper(), "01 06 53 04 FF 9C D6 98")


if __name__ == "__main__":
    unittest.main()

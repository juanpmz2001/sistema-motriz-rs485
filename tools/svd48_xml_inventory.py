#!/usr/bin/env python3
"""Read every SVD48 parameter declared by an official SV-Config XML catalog."""

from __future__ import annotations

import argparse
import csv
import json
import math
import struct
import time
import xml.etree.ElementTree as ET
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

from svd48_lan import read_words, signed16, signed32


CSV_FIELDS = [
    "address",
    "name",
    "display_name",
    "group",
    "subgroup",
    "motor_index",
    "access",
    "driver_type",
    "pc_type",
    "scale",
    "suffix",
    "ok",
    "words_hex",
    "words_unsigned",
    "decoded_value",
    "display_value",
    "enum_value",
    "error",
]


def register_count(driver_type: str) -> int:
    return 2 if driver_type.lower() in {"float", "int32", "uint32"} else 1


def decode_value(driver_type: str, words: list[int]) -> int | float | None:
    kind = driver_type.lower()
    if kind == "float" and len(words) == 2:
        value = struct.unpack(">f", struct.pack(">HH", words[0], words[1]))[0]
        return value if math.isfinite(value) else None
    if kind == "int32" and len(words) == 2:
        return signed32(words[0], words[1])
    if kind == "uint32" and len(words) == 2:
        return (words[0] << 16) | words[1]
    if kind in {"int", "int16"} and len(words) == 1:
        return signed16(words[0])
    if len(words) == 1:
        return words[0]
    return None


def display_value(value: int | float | None, scale_text: str) -> int | float | None:
    if value is None or not scale_text:
        return value
    try:
        return value * float(scale_text)
    except ValueError:
        return value


def enum_value(value: int | float | None, enum_text: str) -> str:
    if not enum_text or not isinstance(value, int):
        return ""
    options = enum_text.split("|")
    return options[value] if 0 <= value < len(options) else ""


def load_catalog(path: Path) -> list[dict[str, str]]:
    root = ET.parse(path).getroot()
    parameters: list[dict[str, str]] = []
    for node in root.findall(".//ParamNode"):
        attrs = {key: value for key, value in node.attrib.items()}
        if not attrs.get("addr") or not attrs.get("name"):
            continue
        try:
            attrs["address_int"] = str(int(attrs["addr"], 16))
        except ValueError:
            continue
        parameters.append(attrs)
    parameters.sort(key=lambda item: (int(item["address_int"]), item["name"]))
    return parameters


def scan(args: argparse.Namespace) -> int:
    catalog_path = Path(args.catalog).resolve()
    parameters = load_catalog(catalog_path)
    results: list[dict[str, Any]] = []
    for index, parameter in enumerate(parameters, start=1):
        address = int(parameter["address_int"])
        driver_type = parameter.get("driver_data_type", "uint16")
        count = register_count(driver_type)
        words, error = read_words(args, args.drive, address, count)
        value = decode_value(driver_type, words or [])
        scale = parameter.get("scale", "")
        shown = display_value(value, scale)
        result = {
            "address": f"0x{address:04X}",
            "address_int": address,
            "name": parameter.get("name", ""),
            "display_name": parameter.get("display_name_en", ""),
            "group": parameter.get("group", ""),
            "subgroup": parameter.get("subgroup", ""),
            "motor_index": parameter.get("motor_index", ""),
            "access": parameter.get("readwrite", ""),
            "driver_type": driver_type,
            "pc_type": parameter.get("pc_data_type", ""),
            "scale": scale,
            "suffix": parameter.get("suffix", ""),
            "minimum": parameter.get("min", ""),
            "maximum": parameter.get("max", ""),
            "default": parameter.get("default", ""),
            "ok": words is not None,
            "words_hex": " ".join(f"0x{word:04X}" for word in words or []),
            "words_unsigned": " ".join(str(word) for word in words or []),
            "decoded_value": value,
            "display_value": shown,
            "enum_value": enum_value(value, parameter.get("enum_list", "")),
            "error": error,
        }
        results.append(result)
        state = "OK" if result["ok"] else "ERR"
        print(
            f"[{index:03d}/{len(parameters):03d}] {state} "
            f"{result['address']} {result['name']} {result['words_hex'] or error}"
        )
        time.sleep(max(0.0, args.delay_ms / 1000.0))

    output_prefix = Path(args.output_prefix)
    output_prefix.parent.mkdir(parents=True, exist_ok=True)
    csv_path = output_prefix.with_suffix(".csv")
    json_path = output_prefix.with_suffix(".json")
    with csv_path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(
            stream,
            fieldnames=CSV_FIELDS,
            extrasaction="ignore",
            lineterminator="\n",
        )
        writer.writeheader()
        writer.writerows(results)
    report = {
        "schema_version": 1,
        "captured_at": datetime.now(timezone.utc).isoformat(),
        "host": args.host,
        "port": args.port,
        "drive_id": args.drive,
        "catalog": str(catalog_path),
        "summary": {
            "parameters": len(results),
            "successful": sum(1 for result in results if result["ok"]),
            "failed": sum(1 for result in results if not result["ok"]),
        },
        "parameters": results,
    }
    json_path.write_text(json.dumps(report, indent=2, ensure_ascii=True) + "\n", encoding="utf-8")
    print(f"CSV {csv_path}")
    print(f"JSON {json_path}")
    print(
        f"SUMMARY parameters={report['summary']['parameters']} "
        f"successful={report['summary']['successful']} failed={report['summary']['failed']}"
    )
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", required=True)
    parser.add_argument("--port", type=int, default=32321)
    parser.add_argument("--timeout-ms", type=int, default=900)
    parser.add_argument("--retries", type=int, default=1)
    parser.add_argument("--token", default="")
    parser.add_argument("--drive", type=int, default=2)
    parser.add_argument("--catalog", required=True)
    parser.add_argument("--delay-ms", type=int, default=50)
    parser.add_argument("--output-prefix", required=True)
    return parser


def main() -> int:
    return scan(build_parser().parse_args())


if __name__ == "__main__":
    raise SystemExit(main())

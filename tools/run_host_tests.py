#!/usr/bin/env python3
"""Configure, build and run the native firmware host-test boundary.

This deliberately does not invoke ESP-IDF or idf.py.  The firmware build and
the host suite have different toolchains and must remain independently runnable.
"""

from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]


def executable_names(name: str) -> tuple[str, ...]:
    return (f"{name}.exe", name) if os.name == "nt" else (name,)


def find_tool(name: str, *, environment_name: str | None = None) -> Path:
    """Find an executable without assuming it was added to the shell PATH."""
    override = os.environ.get(environment_name or f"BOTFARMS_{name.upper()}")
    candidates: list[Path] = []
    if override:
        candidates.append(Path(override))
    located = shutil.which(name)
    if located:
        candidates.append(Path(located))

    idf_tool_roots = [Path.home() / ".espressif"]
    if os.environ.get("IDF_TOOLS_PATH"):
        idf_tool_roots.insert(0, Path(os.environ["IDF_TOOLS_PATH"]))
    for idf_tools in idf_tool_roots:
        for executable in executable_names(name):
            candidates.extend(idf_tools.glob(f"tools/{name}/*/bin/{executable}"))
            candidates.extend(idf_tools.glob(f"tools/{name}/*/{executable}"))

    for candidate in candidates:
        if candidate.is_file():
            return candidate.resolve()
    raise RuntimeError(
        f"Cannot find {name}. Install it or set BOTFARMS_{name.upper()} to its executable path."
    )


def find_vcvars64() -> Path:
    override = os.environ.get("BOTFARMS_HOST_VCVARS64")
    if override and Path(override).is_file():
        return Path(override).resolve()

    roots: list[Path] = []
    program_files_x86 = os.environ.get("ProgramFiles(x86)")
    if program_files_x86:
        vswhere = Path(program_files_x86) / "Microsoft Visual Studio" / "Installer" / "vswhere.exe"
        if vswhere.is_file():
            result = subprocess.run(
                [str(vswhere), "-all", "-products", "*", "-property", "installationPath"],
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=False,
            )
            roots.extend(Path(line) for line in result.stdout.splitlines() if line)
        roots.extend((Path(program_files_x86) / "Microsoft Visual Studio").glob("*/*"))

    for root in roots:
        candidate = root / "VC" / "Auxiliary" / "Build" / "vcvars64.bat"
        if candidate.is_file():
            return candidate.resolve()
    raise RuntimeError(
        "Cannot find MSVC Build Tools. Install the C++ Build Tools workload or set "
        "BOTFARMS_HOST_VCVARS64 to vcvars64.bat."
    )


def run(command: list[str], *, environment: dict[str, str], vcvars: Path | None = None) -> None:
    print("+", subprocess.list2cmdline(command))
    if vcvars is None:
        result = subprocess.run(command, cwd=ROOT, env=environment, check=False)
    else:
        comspec = os.environ.get("ComSpec", "cmd.exe")
        shell_command = f'call "{vcvars}" >nul && {subprocess.list2cmdline(command)}'
        result = subprocess.run(
            shell_command,
            shell=True,
            executable=comspec,
            cwd=ROOT,
            env=environment,
            check=False,
        )
    if result.returncode:
        raise RuntimeError(f"Command failed with exit code {result.returncode}")


def default_build_dir() -> Path:
    # A short temp-rooted path avoids MSVC's legacy object-path limit when the
    # repository is checked out inside a long OneDrive/workspace directory.
    identity = hashlib.sha256(str(ROOT).encode("utf-8")).hexdigest()[:12]
    return Path(tempfile.gettempdir()) / f"botfarms-host-{identity}"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, help="Native-test build directory")
    parser.add_argument(
        "--sanitizers",
        choices=("ON", "OFF"),
        default=os.environ.get("BOTFARMS_HOST_TEST_SANITIZERS", "OFF").upper(),
        help="Enable ASan/UBSan for GNU or Clang host compilers (default: env or OFF)",
    )
    arguments = parser.parse_args(argv)

    cmake = find_tool("cmake")
    ninja = find_tool("ninja")
    ctest = cmake.with_name("ctest.exe" if os.name == "nt" else "ctest")
    if not ctest.is_file():
        raise RuntimeError(f"Cannot find ctest next to CMake: {ctest}")
    build_dir = (arguments.build_dir or default_build_dir()).expanduser().resolve()
    environment = dict(os.environ)
    environment["PATH"] = str(cmake.parent) + os.pathsep + environment.get("PATH", "")
    if arguments.sanitizers == "ON":
        environment["ASAN_OPTIONS"] = (
            environment.get("ASAN_OPTIONS", "") + ":" if environment.get("ASAN_OPTIONS") else ""
        ) + "detect_leaks=0:halt_on_error=1"
        environment["UBSAN_OPTIONS"] = (
            environment.get("UBSAN_OPTIONS", "") + ":" if environment.get("UBSAN_OPTIONS") else ""
        ) + "halt_on_error=1:print_stacktrace=1"

    vcvars: Path | None = None
    configure = [
        str(cmake), "-G", "Ninja", "-S", str(ROOT / "tests"), "-B", str(build_dir),
        f"-DCMAKE_MAKE_PROGRAM={ninja}", "-DCMAKE_BUILD_TYPE=Debug",
        f"-DBOTFARMS_HOST_TEST_SANITIZERS={arguments.sanitizers}",
        f"-DPython3_EXECUTABLE={sys.executable}",
    ]
    if os.name == "nt":
        if arguments.sanitizers == "ON":
            raise RuntimeError("ASan/UBSan host tests require GNU or Clang; MSVC is not supported by this suite.")
        vcvars = find_vcvars64()
        compiler_description = f"MSVC via {vcvars}"
    else:
        compiler = next((shutil.which(candidate) for candidate in ("cc", "gcc", "clang")), None)
        if compiler is None:
            raise RuntimeError("Cannot find a native C compiler (cc, gcc or clang).")
        configure.append(f"-DCMAKE_C_COMPILER={compiler}")
        compiler_description = compiler

    print("BotFarms native host tests")
    print(f"source:   {ROOT}")
    print(f"build:    {build_dir}")
    print(f"cmake:    {cmake}")
    print(f"ctest:    {ctest}")
    print(f"ninja:    {ninja}")
    print(f"python:   {sys.executable}")
    print(f"compiler: {compiler_description}")
    print(f"sanitizers: {arguments.sanitizers}")
    run([str(cmake), "--version"], environment=environment, vcvars=vcvars)
    run([str(ninja), "--version"], environment=environment, vcvars=vcvars)
    run(configure, environment=environment, vcvars=vcvars)
    run([str(cmake), "--build", str(build_dir), "--parallel"], environment=environment, vcvars=vcvars)
    run([str(ctest), "--test-dir", str(build_dir), "-N"], environment=environment, vcvars=vcvars)
    run([str(ctest), "--test-dir", str(build_dir), "--output-on-failure"], environment=environment, vcvars=vcvars)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except RuntimeError as exc:
        print(f"host tests: {exc}", file=sys.stderr)
        raise SystemExit(1)

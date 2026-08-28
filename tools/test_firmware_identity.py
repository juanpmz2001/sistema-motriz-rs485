#!/usr/bin/env python3
"""Regression tests for build-time firmware Git identity generation."""

from __future__ import annotations

import re
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "cmake" / "generate_firmware_identity.cmake"


def run(*args: str, cwd: Path | None = None) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        args,
        cwd=cwd,
        check=True,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )


def read_defines(path: Path) -> dict[str, str]:
    defines: dict[str, str] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        match = re.fullmatch(r"#define\s+(FW_GIT_(?:SHA|DIRTY))\s+(.+)", line)
        if match:
            defines[match.group(1)] = match.group(2).strip().strip('"')
    return defines


def write_fake_git(root: Path, body: str) -> Path:
    """Create a tiny fake Git executable for the CMake identity contract."""
    if os.name == "nt":
        executable = root / "fake-git.cmd"
        executable.write_text("@echo off\r\n" + body, encoding="utf-8")
    else:
        executable = root / "fake-git"
        executable.write_text("#!/bin/sh\n" + body, encoding="utf-8")
        executable.chmod(0o755)
    return executable


class FirmwareIdentityTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.git = shutil.which("git")
        cls.cmake = shutil.which("cmake")
        if cls.git is None or cls.cmake is None:
            raise unittest.SkipTest("git and cmake are required")

    def generate(
        self,
        repository: Path,
        output: Path,
        *,
        sha_override: str = "",
        dirty_override: str = "",
    ) -> dict[str, str]:
        run(
            self.cmake,
            f"-DBOTFARMS_SOURCE_DIR={repository}",
            f"-DBOTFARMS_OUTPUT_FILE={output}",
            f"-DBOTFARMS_GIT_EXECUTABLE={self.git}",
            f"-DBOTFARMS_SHA_OVERRIDE={sha_override}",
            f"-DBOTFARMS_DIRTY_OVERRIDE={dirty_override}",
            "-P",
            str(SCRIPT),
        )
        return read_defines(output)

    def generate_process(
        self,
        repository: Path,
        output: Path,
        *,
        git_executable: str,
        sha_override: str = "",
        dirty_override: str = "",
    ) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            (
                self.cmake,
                f"-DBOTFARMS_SOURCE_DIR={repository}",
                f"-DBOTFARMS_OUTPUT_FILE={output}",
                f"-DBOTFARMS_GIT_EXECUTABLE={git_executable}",
                f"-DBOTFARMS_SHA_OVERRIDE={sha_override}",
                f"-DBOTFARMS_DIRTY_OVERRIDE={dirty_override}",
                "-P",
                str(SCRIPT),
            ),
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )

    def test_same_output_tracks_incremental_worktree_changes(self) -> None:
        with tempfile.TemporaryDirectory(prefix="firmware-identity-") as raw:
            repository = Path(raw) / "repo"
            repository.mkdir()
            run(self.git, "init", "--quiet", cwd=repository)
            run(self.git, "config", "user.email", "tests@example.invalid", cwd=repository)
            run(self.git, "config", "user.name", "Firmware Tests", cwd=repository)
            source = repository / "source.c"
            source.write_text("int value = 1;\n", encoding="utf-8")
            run(self.git, "add", "source.c", cwd=repository)
            run(self.git, "commit", "--quiet", "-m", "fixture", cwd=repository)
            expected_sha = run(self.git, "rev-parse", "HEAD", cwd=repository).stdout.strip()
            output = Path(raw) / "generated" / "identity.h"

            clean = self.generate(repository, output)
            self.assertEqual(clean, {"FW_GIT_SHA": expected_sha, "FW_GIT_DIRTY": "0"})

            source.write_text("int value = 2;\n", encoding="utf-8")
            dirty = self.generate(repository, output)
            self.assertEqual(dirty, {"FW_GIT_SHA": expected_sha, "FW_GIT_DIRTY": "1"})

            run(self.git, "restore", "source.c", cwd=repository)
            clean_again = self.generate(repository, output)
            self.assertEqual(
                clean_again,
                {"FW_GIT_SHA": expected_sha, "FW_GIT_DIRTY": "0"},
            )

    def test_git_queries_scope_safe_directory_to_source_tree(self) -> None:
        with tempfile.TemporaryDirectory(prefix="firmware-identity-") as raw:
            root = Path(raw)
            if os.name == "nt":
                fake_body = (
                    "if not \"%~1\"==\"-c\" exit /b 10\r\n"
                    # cmd.exe splits CMake's safe.directory=<path> argument
                    # at '='; the path is therefore the next batch argument.
                    "if not \"%~2\"==\"safe.directory\" exit /b 11\r\n"
                    "if not \"%~4\"==\"-C\" exit /b 12\r\n"
                    "if not \"%~3\"==\"%~5\" exit /b 13\r\n"
                    "if \"%~6\"==\"rev-parse\" (echo " + "b" * 40 + "& exit /b 0)\r\n"
                    "if \"%~6\"==\"status\" exit /b 0\r\n"
                    "exit /b 14\r\n"
                )
            else:
                fake_body = (
                    "[ \"$1\" = '-c' ] || exit 10\n"
                    "case \"$2\" in safe.directory=*) ;; *) exit 11 ;; esac\n"
                    "[ \"$3\" = '-C' ] || exit 12\n"
                    "[ \"$4\" = \"${2#safe.directory=}\" ] || exit 13\n"
                    "case \"$5\" in\n"
                    "  rev-parse) printf '%s\\n' " + "b" * 40 + "; exit 0 ;;\n"
                    "  status) exit 0 ;;\n"
                    "esac\n"
                    "exit 14\n"
                )
            fake_git = write_fake_git(root, fake_body)
            output = root / "identity.h"

            result = self.generate_process(
                root,
                output,
                git_executable=str(fake_git),
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(
                read_defines(output),
                {"FW_GIT_SHA": "b" * 40, "FW_GIT_DIRTY": "0"},
            )

    def test_explicit_reproducible_overrides_win(self) -> None:
        with tempfile.TemporaryDirectory(prefix="firmware-identity-") as raw:
            output = Path(raw) / "identity.h"
            result = self.generate_process(
                Path(raw),
                output,
                git_executable=str(Path(raw) / "missing-git"),
                sha_override="a" * 40,
                dirty_override="0",
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            observed = read_defines(output)
            self.assertEqual(
                observed,
                {"FW_GIT_SHA": "a" * 40, "FW_GIT_DIRTY": "0"},
            )

    def test_sha_override_without_git_requires_dirty_override(self) -> None:
        with tempfile.TemporaryDirectory(prefix="firmware-identity-") as raw:
            root = Path(raw)
            result = self.generate_process(
                root,
                root / "identity.h",
                git_executable=str(root / "missing-git"),
                sha_override="a" * 40,
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(
                "Unable to verify BOTFARMS_SHA_OVERRIDE against Git HEAD",
                result.stderr,
            )

    def test_sha_override_without_dirty_override_uses_worktree_status(self) -> None:
        with tempfile.TemporaryDirectory(prefix="firmware-identity-") as raw:
            root = Path(raw)
            repository = root / "repo"
            repository.mkdir()
            run(self.git, "init", "--quiet", cwd=repository)
            run(self.git, "config", "user.email", "tests@example.invalid", cwd=repository)
            run(self.git, "config", "user.name", "Firmware Tests", cwd=repository)
            source = repository / "source.c"
            source.write_text("int value = 1;\n", encoding="utf-8")
            run(self.git, "add", "source.c", cwd=repository)
            run(self.git, "commit", "--quiet", "-m", "fixture", cwd=repository)
            expected_sha = run(
                self.git,
                "rev-parse",
                "HEAD",
                cwd=repository,
            ).stdout.strip()
            output = root / "identity.h"

            clean = self.generate(
                repository,
                output,
                sha_override=expected_sha,
            )
            self.assertEqual(
                clean,
                {"FW_GIT_SHA": expected_sha, "FW_GIT_DIRTY": "0"},
            )

            source.write_text("int value = 2;\n", encoding="utf-8")
            dirty = self.generate(
                repository,
                output,
                sha_override=expected_sha,
            )
            self.assertEqual(
                dirty,
                {"FW_GIT_SHA": expected_sha, "FW_GIT_DIRTY": "1"},
            )

    def test_sha_override_without_dirty_override_must_match_git_head(self) -> None:
        with tempfile.TemporaryDirectory(prefix="firmware-identity-") as raw:
            root = Path(raw)
            repository = root / "repo"
            repository.mkdir()
            run(self.git, "init", "--quiet", cwd=repository)
            run(self.git, "config", "user.email", "tests@example.invalid", cwd=repository)
            run(self.git, "config", "user.name", "Firmware Tests", cwd=repository)
            source = repository / "source.c"
            source.write_text("int value = 1;\n", encoding="utf-8")
            run(self.git, "add", "source.c", cwd=repository)
            run(self.git, "commit", "--quiet", "-m", "fixture", cwd=repository)

            result = self.generate_process(
                repository,
                root / "identity.h",
                git_executable=self.git,
                sha_override="a" * 40,
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(
                "BOTFARMS_SHA_OVERRIDE does not match Git HEAD",
                result.stderr,
            )

    def test_sha_override_with_failed_git_status_cannot_be_reported_as_clean(self) -> None:
        with tempfile.TemporaryDirectory(prefix="firmware-identity-") as raw:
            root = Path(raw)
            if os.name == "nt":
                fake_body = (
                    "if \"%~6\"==\"rev-parse\" (echo " + "a" * 40 + "& exit /b 0)\r\n"
                    "if \"%~6\"==\"status\" exit /b 2\r\n"
                    "exit /b 2\r\n"
                )
            else:
                fake_body = (
                    "case \" $* \" in\n"
                    "  *' rev-parse --verify HEAD '*) printf '%s\\n' " + "a" * 40 + "; exit 0 ;;\n"
                    "  *' status --porcelain --untracked-files=normal '*) exit 2 ;;\n"
                    "esac\n"
                    "exit 2\n"
                )
            fake_git = write_fake_git(root, fake_body)
            result = self.generate_process(
                root,
                root / "identity.h",
                git_executable=str(fake_git),
                sha_override="a" * 40,
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(
                "Unable to determine firmware worktree dirty state",
                result.stderr,
            )

    def test_unknown_sha_cannot_be_reported_as_clean(self) -> None:
        with tempfile.TemporaryDirectory(prefix="firmware-identity-") as raw:
            root = Path(raw)
            result = self.generate_process(
                root,
                root / "identity.h",
                git_executable=str(root / "missing-git"),
                sha_override="UNKNOWN",
                dirty_override="0",
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(
                "UNKNOWN firmware Git SHA cannot be reported as a clean build identity",
                result.stderr,
            )

    def test_invalid_override_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory(prefix="firmware-identity-") as raw:
            root = Path(raw)
            result = self.generate_process(
                root,
                root / "identity.h",
                git_executable=self.git,
                sha_override="not-a-sha",
                dirty_override="0",
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("must be 40 hex characters", result.stderr)


if __name__ == "__main__":
    unittest.main()

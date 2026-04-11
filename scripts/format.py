#!/usr/bin/env python3
"""Run clang-format and buildifier on changed files. Works on both Linux/macOS and Windows."""

import os
import shutil
import subprocess
import sys
from pathlib import Path

project_root = Path(__file__).resolve().parent.parent
os.chdir(project_root)

subprocess.run(["git", "fetch", "origin", "main"], check=True)

clang_format = shutil.which("clang-format-20") or shutil.which("clang-format")
git_clang_format = shutil.which("git-clang-format-20") or shutil.which("git-clang-format")

if git_clang_format and clang_format:
    subprocess.run(
        [git_clang_format, "--binary", clang_format, "-f", "origin/main"],
    )
else:
    print("WARNING: git-clang-format or clang-format not found, skipping", file=sys.stderr)

buildifier = shutil.which("buildifier")
if buildifier:
    subprocess.run([buildifier, "-r", "./"], check=True)
else:
    print("WARNING: buildifier not found, skipping", file=sys.stderr)

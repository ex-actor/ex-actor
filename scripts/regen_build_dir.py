#!/usr/bin/env python3
"""Regenerate the cmake build directory. Works on both Linux/macOS and Windows."""

import os
import platform
import re
import shutil
import stat
import subprocess
import sys
from pathlib import Path


def force_remove_readonly(func, path, exc_info):
    """Handle read-only files (e.g. git pack files) on Windows."""
    os.chmod(path, stat.S_IWRITE)
    func(path)

project_root = Path(__file__).resolve().parent.parent
os.chdir(project_root)

# Clean previous build artifacts
build_dir = project_root / "build"
if build_dir.exists():
    shutil.rmtree(build_dir, onerror=force_remove_readonly)
compile_commands = project_root / "compile_commands.json"
compile_commands.unlink(missing_ok=True)

os.environ["CPM_SOURCE_CACHE"] = str(Path.home() / ".cache" / "CPM")

cmake_args = [
    "cmake", "-S", ".", "-B", "build",
    "-DCMAKE_EXPORT_COMPILE_COMMANDS=1",
]

if platform.system() == "Windows":
    cmake_args += ["-G", "Visual Studio 17 2022"]
else:
    cmake_args += ["-G", "Ninja Multi-Config"]
    if shutil.which("ccache"):
        cmake_args += [
            "-DCMAKE_C_COMPILER_LAUNCHER=ccache",
            "-DCMAKE_CXX_COMPILER_LAUNCHER=ccache",
        ]

cmake_args += sys.argv[1:]

print("+", " ".join(cmake_args), flush=True)
subprocess.run(cmake_args, check=True)

# Copy compile_commands.json to project root (if generated)
src = build_dir / "compile_commands.json"
if src.exists():
    shutil.copy2(src, compile_commands)

    # Remove args not supported by clangd
    with open(compile_commands, "r") as f:
        text = f.read()
    text = re.sub(r"-fconcepts-diagnostics-depth=\d*", "", text)
    with open(compile_commands, "w") as f:
        f.write(text)

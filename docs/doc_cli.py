#!/usr/bin/env python3
"""Build or serve project documentation with mkdocs. Works on both Linux/macOS and Windows."""

import argparse
import shutil
import subprocess
from pathlib import Path

docs_dir = Path(__file__).resolve().parent
project_root = docs_dir.parent
contents_dir = docs_dir / "contents"

parser = argparse.ArgumentParser(description="Build or serve project documentation.")
parser.add_argument("command", choices=["serve", "build"])
parser.add_argument("-p", "--port", type=int, default=2023, help="Port for serve (default: 2023)")
args = parser.parse_args()

DEPS = (
    "mkdocs-d2-plugin,"
    "mkdocs-material[imaging],"
    "mkdocs-add-number-plugin,"
    "mkdocs-enumerate-headings-plugin,"
    "mkdocs-git-revision-date-localized-plugin,"
    "mkdocs-git-committers-plugin-2,"
    "click<8.3"
)

if args.command == "serve":
    subprocess.run(
        [
            "uvx", "--python", "3.10", "--with", DEPS, "mkdocs", "serve",
            "-a", f"0.0.0.0:{args.port}",
            "--livereload",
            "--watch", str(project_root / "README.md"),
            "--watch", str(project_root / "CONTRIBUTING.md"),
            "--watch", str(project_root / "assets"),
        ],
        cwd=docs_dir,
        check=True,
    )
elif args.command == "build":
    subprocess.run(
        ["uvx", "--python", "3.10", "--with", DEPS, "mkdocs", "build"],
        cwd=docs_dir,
        check=True,
    )
    google_verification = docs_dir / "googlec013d979e435280a.html"
    if google_verification.exists():
        shutil.copy2(google_verification, docs_dir / "site" / google_verification.name)

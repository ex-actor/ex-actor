#!/usr/bin/env python3
"""Build or serve project documentation with mkdocs. Works on both Linux/macOS and Windows."""

import argparse
import re
import shutil
import subprocess
import sys
from pathlib import Path

docs_dir = Path(__file__).resolve().parent
project_root = docs_dir.parent
contents_dir = docs_dir / "contents"

# Copy assets
assets_dst = contents_dir / "assets"
if assets_dst.exists():
    shutil.rmtree(assets_dst)
shutil.copytree(project_root / "assets", assets_dst)

# Copy README.md -> contents/index.md
index_md = contents_dir / "index.md"
text = (project_root / "README.md").read_text(encoding="utf-8")
text = "# Introduction\n" + text
text = re.sub(
    r"<!-- GITHUB README ONLY START -->.*?<!-- GITHUB README ONLY END -->",
    "",
    text,
    flags=re.DOTALL,
)
index_md.write_text(text, encoding="utf-8")

# Copy CONTRIBUTING.md -> contents/contributing.md
contributing_md = contents_dir / "contributing.md"
shutil.copy2(project_root / "CONTRIBUTING.md", contributing_md)

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
    "mkdocs-git-committers-plugin-2"
)

if args.command == "serve":
    subprocess.run(
        ["uvx", "--with", DEPS, "mkdocs", "serve", "-a", f"0.0.0.0:{args.port}"],
        cwd=docs_dir,
        check=True,
    )
elif args.command == "build":
    subprocess.run(
        ["uvx", "--with", DEPS, "mkdocs", "build"],
        cwd=docs_dir,
        check=True,
    )
    google_verification = docs_dir / "googlec013d979e435280a.html"
    if google_verification.exists():
        shutil.copy2(google_verification, docs_dir / "site" / google_verification.name)

"""MkDocs hook: re-copy root files (README, CONTRIBUTING, assets) into docs/contents on each build.

Only writes when content has actually changed, to avoid triggering infinite rebuild loops.
"""

import filecmp
import re
import shutil
from pathlib import Path

docs_dir = Path(__file__).resolve().parent.parent
project_root = docs_dir.parent
contents_dir = docs_dir / "contents"


def _write_if_changed(path: Path, new_text: str) -> None:
    if path.exists() and path.read_text(encoding="utf-8") == new_text:
        return
    path.write_text(new_text, encoding="utf-8")


def _copy_if_changed(src: Path, dst: Path) -> None:
    if dst.exists() and filecmp.cmp(src, dst, shallow=False):
        return
    shutil.copy2(src, dst)


def _copytree_if_changed(src_dir: Path, dst_dir: Path) -> None:
    """Sync src_dir into dst_dir, only writing files whose content differs."""
    src_files = {p.relative_to(src_dir): p for p in src_dir.rglob("*") if p.is_file()}
    dst_files = {p.relative_to(dst_dir): p for p in dst_dir.rglob("*") if p.is_file()} if dst_dir.exists() else {}

    changed = False
    for rel, src_path in src_files.items():
        dst_path = dst_dir / rel
        if rel in dst_files and filecmp.cmp(src_path, dst_path, shallow=False):
            continue
        dst_path.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src_path, dst_path)
        changed = True

    # Remove files in dst that no longer exist in src
    for rel in dst_files:
        if rel not in src_files:
            (dst_dir / rel).unlink()
            changed = True

    return changed


def on_pre_build(**kwargs):
    # Copy assets
    _copytree_if_changed(project_root / "assets", contents_dir / "assets")

    # Copy README.md -> contents/index.md
    text = (project_root / "README.md").read_text(encoding="utf-8")
    text = "# Introduction\n" + text
    text = re.sub(
        r"<!-- GITHUB README ONLY START -->.*?<!-- GITHUB README ONLY END -->",
        "",
        text,
        flags=re.DOTALL,
    )
    _write_if_changed(contents_dir / "index.md", text)

    # Copy CONTRIBUTING.md -> contents/contributing.md
    _copy_if_changed(project_root / "CONTRIBUTING.md", contents_dir / "contributing.md")

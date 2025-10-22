#!/usr/bin/env python3
"""
Extract and test code blocks from markdown documentation.

This script automates testing of code examples in documentation by:
1. Extracting all <!-- doc test start -->/<!-- doc test end --> blocks from markdown files
2. Creating a temporary project based on test/import_test/cmake_cpm
3. Configuring CPM to use the current source code (not from GitHub)
4. Saving extracted code as separate test files
5. Adding them as CMake tests and running them

Usage:
    # Test all doc blocks in the project
    python3 test/doc_test.py
    
    # Keep the temporary directory for inspection
    python3 test/doc_test.py --keep-temp
    
    # Search only specific directory
    python3 test/doc_test.py --root docs/
    
    # Verbose test output
    python3 test/doc_test.py --verbose

Requirements:
    - cmake (>= 3.25.0)
    - ninja
    - wget
    - C++20 compiler
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import List, Tuple


def find_markdown_files(root_dir: Path) -> List[Path]:
    """Find all markdown files in the directory tree."""
    md_files = []
    for pattern in ['*.md', '**/*.md']:
        md_files.extend(root_dir.glob(pattern))
    return sorted(set(md_files))


def extract_doc_test_blocks(md_file: Path) -> List[Tuple[str, int, str]]:
    """
    Extract code blocks between <!-- doc test start --> and <!-- doc test end -->.
    Returns list of (code_content, line_number, wrapper_script) tuples.
    wrapper_script is empty string if not specified.
    """
    content = md_file.read_text(encoding='utf-8')
    blocks = []
    
    # Pattern to match doc test blocks with optional wrapper_script parameter
    pattern = r'<!-- doc test start(?:,\s*wrapper_script:\s*(\S+))?\s*-->\s*```(?:cpp|c\+\+)?\s*\n(.*?)```\s*<!-- doc test end -->'
    
    for match in re.finditer(pattern, content, re.DOTALL):
        wrapper_script = match.group(1) if match.group(1) else ""
        code = match.group(2)
        # Find line number
        line_num = content[:match.start()].count('\n') + 1
        blocks.append((code, line_num, wrapper_script))
    
    return blocks


def copy_template_project(template_dir: Path, dest_dir: Path):
    """Copy the cmake_cpm template to the destination directory."""
    # Copy all files except build directories
    for item in template_dir.iterdir():
        if item.name in ['build', 'build_ci.sh', 'build.sh', 'main.cc']:
            continue
        if item.is_file():
            shutil.copy2(item, dest_dir / item.name)
        elif item.is_dir():
            shutil.copytree(item, dest_dir / item.name)


def create_cmake_lists(test_files: List[Tuple[str, str]], source_dir: Path, cmake_file: Path):
    """Create CMakeLists.txt with all test executables.
    
    Args:
        test_files: List of (test_filename, wrapper_script) tuples
        source_dir: Path to the ex_actor source directory
        cmake_file: Path where CMakeLists.txt should be written
    """
    cmake_content = f"""cmake_minimum_required(VERSION 3.25.0 FATAL_ERROR)
project(doc_tests)

enable_testing()

# For more information on how to add CPM to your project, see: https://github.com/cpm-cmake/CPM.cmake#adding-cpm
include(CPM.cmake)

CPMAddPackage(
  NAME ex_actor
  SOURCE_DIR "{source_dir}"
  OPTIONS "EX_ACTOR_BUILD_TESTS OFF"
)

"""
    
    for test_file, wrapper_script in test_files:
        test_name = Path(test_file).stem
        
        if wrapper_script:
            # Test with wrapper script (similar to multi_process_test)
            wrapper_name = Path(wrapper_script).name
            cmake_content += f"""
add_executable({test_name} {test_file})
target_link_libraries({test_name} ex_actor::ex_actor)
configure_file(
  ${{CMAKE_CURRENT_SOURCE_DIR}}/{wrapper_name}
  ${{CMAKE_CURRENT_BINARY_DIR}}/{wrapper_name}
  COPYONLY
)
add_test(
  NAME {test_name}
  COMMAND {wrapper_name} $<TARGET_FILE:{test_name}>
)
set_tests_properties({test_name} PROPERTIES TIMEOUT 3)
"""
        else:
            # Regular test without wrapper script
            cmake_content += f"""
add_executable({test_name} {test_file})
target_link_libraries({test_name} ex_actor::ex_actor)
add_test(NAME {test_name} COMMAND {test_name})
set_tests_properties({test_name} PROPERTIES TIMEOUT 3)
"""
    
    cmake_file.write_text(cmake_content)


def setup_cpm(temp_dir: Path):
    """Download CPM.cmake to the temporary directory."""
    cpm_file = temp_dir / "CPM.cmake"
    url = "https://github.com/cpm-cmake/CPM.cmake/releases/latest/download/get_cpm.cmake"
    
    print(f"Downloading CPM.cmake from {url}...")
    subprocess.run(
        ["wget", "-O", str(cpm_file), url],
        check=True,
        cwd=temp_dir
    )


def build_and_test(temp_dir: Path, verbose: bool = False):
    """Build the project and run tests."""
    build_dir = temp_dir / "build"
    build_dir.mkdir(exist_ok=True)
    
    print("\n=== Configuring CMake ===")
    cmake_cmd = [
        "cmake", "-S", str(temp_dir), "-B", str(build_dir),
        "-G", "Ninja Multi-Config"
    ]
    # Set CPM_SOURCE_CACHE environment variable
    env = os.environ.copy()
    env['CPM_SOURCE_CACHE'] = os.path.expanduser('~/.cache/CPM')
    subprocess.run(cmake_cmd, check=True, env=env)
    
    print("\n=== Building ===")
    build_cmd = [
        "cmake", "--build", str(build_dir), "--config", "Release"
    ]
    subprocess.run(build_cmd, check=True)
    
    print("\n=== Running Tests ===")
    test_cmd = [
        "ctest", "-C", "Release", "--output-on-failure"
    ]
    if verbose:
        test_cmd.append("-V")
    
    subprocess.run(test_cmd, check=True, cwd=build_dir)


def sanitize_filename(md_file: Path, block_index: int) -> str:
    """Create a safe filename from markdown file path and block index."""
    # Get relative path without extension
    name_parts = md_file.stem.replace('/', '_').replace('\\', '_')
    if md_file.parent.name not in ['.', '']:
        parent_parts = str(md_file.parent).replace('/', '_').replace('\\', '_')
        return f"test_{parent_parts}_{name_parts}_{block_index}.cc"
    return f"test_{name_parts}_{block_index}.cc"


def main():
    parser = argparse.ArgumentParser(
        description='Extract and test documentation code blocks'
    )
    parser.add_argument(
        '--root',
        type=Path,
        help='Root directory to search for markdown files (default: project root)'
    )
    parser.add_argument(
        '--keep-temp',
        action='store_true',
        help='Keep temporary directory after testing'
    )
    parser.add_argument(
        '--verbose',
        '-v',
        action='store_true',
        help='Verbose test output'
    )
    
    args = parser.parse_args()
    
    # Determine project root (3 levels up from this script: test/doc_test.py -> project_root)
    script_dir = Path(__file__).parent.resolve()
    project_root = script_dir.parent
    
    if args.root:
        search_root = args.root.resolve()
    else:
        search_root = project_root
    
    template_dir = project_root / "test" / "import_test" / "cmake_cpm"
    
    print(f"Project root: {project_root}")
    print(f"Searching for markdown files in: {search_root}")
    print(f"Template directory: {template_dir}")
    
    # Find all markdown files
    md_files = find_markdown_files(search_root)
    print(f"\nFound {len(md_files)} markdown file(s)")
    
    # Extract all doc test blocks
    all_blocks = []
    for md_file in md_files:
        blocks = extract_doc_test_blocks(md_file)
        if blocks:
            print(f"  {md_file.relative_to(search_root)}: {len(blocks)} block(s)")
            for i, (code, line_num, wrapper_script) in enumerate(blocks):
                all_blocks.append((md_file, i, code, line_num, wrapper_script))
    
    if not all_blocks:
        print("\nNo doc test blocks found!")
        return 0
    
    print(f"\nTotal: {len(all_blocks)} test block(s) to process")
    
    # Create temporary directory
    if args.keep_temp:
        temp_dir = Path(tempfile.mkdtemp(prefix='ex_actor_doc_test_'))
        print(f"\nTemporary directory: {temp_dir}")
    else:
        temp_dir_obj = tempfile.TemporaryDirectory(prefix='ex_actor_doc_test_')
        temp_dir = Path(temp_dir_obj.name)
        print(f"\nTemporary directory: {temp_dir}")
    
    try:
        # Copy template project
        print("\nCopying template project...")
        copy_template_project(template_dir, temp_dir)
        
        # Setup CPM
        setup_cpm(temp_dir)
        
        # Save extracted code blocks as test files
        test_files = []
        wrapper_scripts = set()  # Track unique wrapper scripts to copy
        print("\nCreating test files:")
        for md_file, block_idx, code, line_num, wrapper_script in all_blocks:
            filename = sanitize_filename(md_file.relative_to(search_root), block_idx)
            test_file = temp_dir / filename
            
            test_file.write_text(code, encoding='utf-8')
            test_files.append((filename, wrapper_script))
            
            rel_md = md_file.relative_to(search_root)
            if wrapper_script:
                print(f"  {filename} (from {rel_md}:{line_num}, wrapper: {wrapper_script})")
                wrapper_scripts.add(wrapper_script)
            else:
                print(f"  {filename} (from {rel_md}:{line_num})")
        
        # Copy wrapper scripts to temp directory
        if wrapper_scripts:
            print("\nCopying wrapper scripts:")
            for wrapper_script in wrapper_scripts:
                wrapper_src = project_root / wrapper_script
                wrapper_dst = temp_dir / Path(wrapper_script).name
                if wrapper_src.exists():
                    shutil.copy2(wrapper_src, wrapper_dst)
                    # Make sure the script is executable
                    wrapper_dst.chmod(0o755)
                    print(f"  {wrapper_script}")
                else:
                    print(f"  Warning: {wrapper_script} not found!", file=sys.stderr)
        
        # Create CMakeLists.txt
        print("\nGenerating CMakeLists.txt...")
        create_cmake_lists(test_files, project_root, temp_dir / "CMakeLists.txt")
        
        # Build and test
        build_and_test(temp_dir, args.verbose)
        
        print("\n✅ All doc tests passed!")
        
        if args.keep_temp:
            print(f"\nTemporary directory preserved at: {temp_dir}")
        
        return 0
        
    except subprocess.CalledProcessError as e:
        print(f"\n❌ Error: Command failed with exit code {e.returncode}", file=sys.stderr)
        if args.keep_temp:
            print(f"Temporary directory preserved at: {temp_dir}", file=sys.stderr)
        return 1
    except Exception as e:
        print(f"\n❌ Error: {e}", file=sys.stderr)
        if args.keep_temp:
            print(f"Temporary directory preserved at: {temp_dir}", file=sys.stderr)
        return 1
    finally:
        if not args.keep_temp:
            temp_dir_obj.cleanup()


if __name__ == "__main__":
    sys.exit(main())

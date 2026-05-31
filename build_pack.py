#!/usr/bin/env python3
"""
CoreTurboScheduler Build & Pack Script
Correct workflow: Compile -> Copy binary to magisk/ -> Pack ZIP

Usage:
    python build_pack.py              # Full build + pack
    python build_pack.py build        # Build only
    python build_pack.py pack         # Pack only (binary must exist)
    python build_pack.py clean        # Clean all

Environment:
    ANDROID_NDK    Path to Android NDK (required for build)
"""
from datetime import datetime
import subprocess
import sys
import os
import platform
import shutil

# ============ Configuration ============
PROJECT_NAME = "CoreTurboScheduler"
BINARY_NAME = "CoreTurboScheduler"
MODULE_ID = "CoreTurboScheduler"

now = datetime.now()
DATE_CODE = now.strftime("%Y%m%d")
TIME_STR = now.strftime("%Y %m %d: %H:%M:%S")

# Paths
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
MAGISK_DIR = os.path.join(SCRIPT_DIR, "magisk")
SRC_FILE = os.path.join(SCRIPT_DIR, "src", "main.cpp")
OUTPUT_DIR = os.path.join(SCRIPT_DIR, "output")

# NDK detection
NDK_PATHS = {
    "Windows": [
        "D:/Android-NDK",
        "C:/Users/%USERNAME%/AppData/Local/Android/Sdk/ndk",
        "C:/Android-NDK",
    ],
    "Linux": [
        os.path.expanduser("~/Android/Sdk/ndk"),
        "/opt/android-ndk",
        os.path.expanduser("~/android-ndk"),
    ],
    "Darwin": [
        os.path.expanduser("~/Library/Android/sdk/ndk"),
        "/opt/android-ndk",
        os.path.expanduser("~/android-ndk"),
    ],
}

# ============ Logging ============
def log(msg):
    print(f"[{TIME_STR}] {msg}")

def error(msg):
    print(f"[ERROR] {msg}", file=sys.stderr)

def success(msg):
    print(f"[OK] {msg}")

# ============ NDK Detection ============
def find_ndk():
    """Find Android NDK installation"""
    # Check environment variable first
    env_ndk = os.environ.get("ANDROID_NDK", os.environ.get("ANDROID_NDK_HOME", ""))
    if env_ndk and os.path.exists(env_ndk):
        return env_ndk

    # Check predefined paths
    system = platform.system()
    for p in NDK_PATHS.get(system, []):
        if "%USERNAME%" in p:
            p = p.replace("%USERNAME%", os.environ.get("USERNAME", ""))
        if os.path.exists(p):
            subdirs = [d for d in os.listdir(p) if os.path.isdir(os.path.join(p, d)) and d[0].isdigit()]
            if subdirs:
                return os.path.join(p, sorted(subdirs)[-1])
            return p
    return None

def get_clang(ndk_root):
    """Get clang++ path from NDK"""
    system = platform.system()
    hosts = {"Windows": "windows-x86_64", "Linux": "linux-x86_64", "Darwin": "darwin-x86_64"}
    host = hosts.get(system, "linux-x86_64")
    clang_name = "clang++.exe" if system == "Windows" else "clang++"
    clang_path = os.path.join(ndk_root, "toolchains", "llvm", "prebuilt", host, "bin", clang_name)
    return clang_path if os.path.exists(clang_path) else None

def get_sysroot(ndk_root):
    """Get sysroot path from NDK"""
    system = platform.system()
    hosts = {"Windows": "windows-x86_64", "Linux": "linux-x86_64", "Darwin": "darwin-x86_64"}
    host = hosts.get(system, "linux-x86_64")
    return os.path.join(ndk_root, "toolchains", "llvm", "prebuilt", host, "sysroot")

# ============ Build ============
def build_binary():
    """Build the binary for Android arm64"""
    ndk = find_ndk()
    if not ndk:
        log("=" * 50)
        error("Android NDK not found!")
        log("Please install Android NDK and set ANDROID_NDK environment variable.")
        log("Example: export ANDROID_NDK=/path/to/android-ndk")
        log("=" * 50)
        return False

    log(f"Using NDK: {ndk}")

    clang = get_clang(ndk)
    if not clang:
        error("clang++ not found in NDK!")
        return False

    sysroot = get_sysroot(ndk)
    include_dir = os.path.join(SCRIPT_DIR, "include")

    os.makedirs(OUTPUT_DIR, exist_ok=True)

    # Compiler flags (optimized for Android, learned from CoreTaskOptimizer)
    cpp_flags = [
        f"--sysroot={sysroot}",
        "--target=aarch64-linux-android29",
        "-std=c++23",
        "-static",          # Static linking for standalone binary
        "-s",               # Strip symbol table
        "-O3",              # Max optimization
        "-flto",            # Link-time optimization
        "-ffast-math",      # Fast math
        "-funroll-loops",   # Loop unrolling
        "-finline-functions",  # Function inlining
        "-fomit-frame-pointer",  # Omit frame pointer
        "-Wall", "-Wextra", "-Wshadow",  # Warnings
        "-fno-rtti",        # No RTTI (smaller binary)
        "-fno-threadsafe-statics",  # Optimization for static init
        "-fvisibility=hidden",  # Hide symbols
        "-ffunction-sections",  # Remove unused functions (learned from CoreTaskOptimizer)
        "-fdata-sections",      # Remove unused data
        "-Wl,--gc-sections",    # Garbage collect unused sections
        f"-I{include_dir}",
    ]

    output_bin = os.path.join(OUTPUT_DIR, BINARY_NAME)
    command = [clang] + cpp_flags + [SRC_FILE, "-o", output_bin]

    log(f"Build command: {' '.join(command)}")
    log("Compiling...")

    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode != 0:
        error("Compilation failed!")
        if result.stderr:
            log(result.stderr)
        return False

    # Strip binary further
    strip_tool = os.path.join(os.path.dirname(clang), "llvm-strip" if platform.system() != "Windows" else "llvm-strip.exe")
    if os.path.exists(strip_tool):
        subprocess.run([strip_tool, "--strip-all", output_bin], capture_output=True)

    size_kb = os.path.getsize(output_bin) / 1024
    success(f"Build successful! ({size_kb:.1f} KB)")
    success(f"Output: {output_bin}")
    return True

# ============ Copy Binary to Magisk ============
def copy_to_magisk():
    """Copy compiled binary and config to magisk directory"""
    src_bin = os.path.join(OUTPUT_DIR, BINARY_NAME)

    if not os.path.exists(src_bin):
        error(f"Binary not found: {src_bin}")
        error("Run 'python build_pack.py build' first, or manually compile.")
        return False

    # Copy binary
    dst_bin = os.path.join(MAGISK_DIR, BINARY_NAME)
    shutil.copy2(src_bin, dst_bin)
    log(f"Copied binary to: {dst_bin}")

    # Copy config.json
    src_config = os.path.join(SCRIPT_DIR, "Json", "config.json")
    dst_config = os.path.join(MAGISK_DIR, "config.json")
    shutil.copy2(src_config, dst_config)
    log(f"Copied config to: {dst_config}")

    # Update module.prop versionCode
    prop_file = os.path.join(MAGISK_DIR, "module.prop")
    if os.path.exists(prop_file):
        with open(prop_file, "r", encoding="utf-8") as f:
            lines = f.readlines()
        with open(prop_file, "w", encoding="utf-8") as f:
            for line in lines:
                if line.startswith("versionCode="):
                    f.write(f"versionCode={DATE_CODE}\n")
                else:
                    f.write(line)
        log(f"Updated versionCode: {DATE_CODE}")

    success("Magisk module files prepared.")
    return True

# ============ Pack Magisk ZIP ============
def pack_magisk():
    """Create Magisk module ZIP"""
    # Check if binary exists in magisk dir
    magisk_bin = os.path.join(MAGISK_DIR, BINARY_NAME)
    if not os.path.exists(magisk_bin):
        error(f"Binary not found in magisk/: {BINARY_NAME}")
        log("Please run 'python build_pack.py build' first to compile.")
        return False

    zip_name = f"{PROJECT_NAME}-v3.2-{DATE_CODE}.zip"
    zip_path = os.path.join(OUTPUT_DIR, zip_name)
    os.makedirs(OUTPUT_DIR, exist_ok=True)

    log(f"Creating Magisk module: {zip_name}")

    # Create ZIP from magisk directory (contents only, not the magisk/ folder itself)
    # Magisk expects files at root of ZIP
    result = subprocess.run(
        ["zip", "-r", zip_path, "."],
        cwd=MAGISK_DIR,
        capture_output=True,
        text=True
    )

    if result.returncode == 0:
        size_kb = os.path.getsize(zip_path) / 1024
        success(f"Magisk module created! ({size_kb:.1f} KB)")
        success(f"Output: {zip_path}")
        return True
    else:
        error(f"Failed to create ZIP: {result.stderr}")
        return False

# ============ Clean ============
def clean():
    """Clean build artifacts"""
    # Remove output directory
    if os.path.exists(OUTPUT_DIR):
        shutil.rmtree(OUTPUT_DIR)
        log("Cleaned output/")

    # Remove binary from magisk dir
    magisk_bin = os.path.join(MAGISK_DIR, BINARY_NAME)
    if os.path.exists(magisk_bin):
        os.remove(magisk_bin)
        log(f"Removed {magisk_bin}")

    success("Clean complete.")

# ============ Main ============
def show_usage():
    print("""
Usage: python build_pack.py [command]

Commands:
    (none)     Full workflow: build -> copy -> pack
    build      Compile binary only
    copy       Copy binary and config to magisk/
    pack       Create Magisk ZIP (binary must be in magisk/)
    clean      Remove all build artifacts
    help       Show this help

Environment:
    ANDROID_NDK    Path to Android NDK (required for build)

Examples:
    # Full workflow
    export ANDROID_NDK=/path/to/ndk
    python build_pack.py

    # Build only (for manual testing)
    python build_pack.py build

    # If you compiled manually, just pack
    cp CoreTurboScheduler magisk/
    python build_pack.py pack
""")

def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else "full"

    if cmd in ("full", ""):
        log("=" * 50)
        log("Step 1/3: Building binary...")
        log("=" * 50)
        if not build_binary():
            sys.exit(1)

        log("")
        log("=" * 50)
        log("Step 2/3: Copying to magisk/...")
        log("=" * 50)
        if not copy_to_magisk():
            sys.exit(1)

        log("")
        log("=" * 50)
        log("Step 3/3: Packing Magisk ZIP...")
        log("=" * 50)
        if not pack_magisk():
            sys.exit(1)

        log("")
        log("=" * 50)
        log("ALL DONE!")
        log("=" * 50)

    elif cmd == "build":
        if not build_binary():
            sys.exit(1)

    elif cmd == "copy":
        if not copy_to_magisk():
            sys.exit(1)

    elif cmd == "pack":
        if not pack_magisk():
            sys.exit(1)

    elif cmd == "clean":
        clean()

    elif cmd in ("help", "-h", "--help"):
        show_usage()

    else:
        error(f"Unknown command: {cmd}")
        show_usage()
        sys.exit(1)

if __name__ == "__main__":
    main()

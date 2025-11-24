#!/usr/bin/env python3
"""
Script to prepare embit library for MicroPython by removing Python3-only files.

This script removes files that are not needed for MicroPython:
- embit/util/ctypes_secp256k1.py (uses ctypes, not available in MicroPython)
- embit/util/pyhashlib.py (uses standard library hashlib, use uhashlib instead)

Usage:
    python3 ext_mod/embit/prepare_embit.py

This should be run after cloning/updating the embit submodule.
"""

import os
import sys

# Files to remove (Python3-only, not needed for MicroPython)
FILES_TO_REMOVE = [
    "embit/util/ctypes_secp256k1.py",
]

def main():
    script_dir = os.path.dirname(os.path.abspath(__file__))
    embit_dir = os.path.join(script_dir, "embit", "src")
    
    if not os.path.exists(embit_dir):
        print(f"Error: embit directory not found at {embit_dir}")
        print("Please ensure the embit submodule is initialized:")
        print("  git submodule update --init --recursive ext_mod/embit/embit")
        sys.exit(1)
    
    removed_count = 0
    for file_path in FILES_TO_REMOVE:
        full_path = os.path.join(embit_dir, file_path)
        if os.path.exists(full_path):
            os.remove(full_path)
            print(f"Removed: {file_path}")
            removed_count += 1
        else:
            print(f"Not found (already removed?): {file_path}")
    
    if removed_count > 0:
        print(f"\nRemoved {removed_count} Python3-only file(s).")
        print("Embit is now prepared for MicroPython.")
    else:
        print("\nNo files to remove. Embit is already prepared for MicroPython.")

if __name__ == "__main__":
    main()


# Embit library manifest for MicroPython
# This manifest includes the embit package for freezing into firmware

# Include the embit package from src/embit directory
# Note: Python3-only files (ctypes_secp256k1.py, pyhashlib.py) should be
# removed using prepare_embit.py script before building, or they will be
# included but won't cause issues if not imported.
package(
    "embit",
    base_path="embit/src",  # Relative to this manifest file's directory
    opt=3  # Optimization level 3 for frozen modules
)


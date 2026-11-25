"""Run the existing command guest and check that its cleanup ran."""

import subprocess
import sys


if __name__ == "__main__":
    result = subprocess.run([*sys.argv[1:], "demo"], stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True)
    print(result.stdout, end="")
    if result.returncode:
        raise SystemExit(result.returncode)
    if "guest cleanup complete" not in result.stdout:
        raise SystemExit("Guest did not report cleanup")

# SPDX-License-Identifier: LGPL-2.1-or-later
import sys
import os

def main():
    if len(sys.argv) < 2:
        print("FCGEO/1 {\"type\":\"error\",\"code\":\"missing_arg\",\"message\":\"Usage: GeometryWorker.py <request.json>\"}")
        sys.exit(1)
    
    request_path = sys.argv[1]
    import Part
    if hasattr(Part, "_runGeometryWorker"):
        res = Part._runGeometryWorker(request_path)
        sys.exit(res)
    else:
        print(f"FCGEO/1 {{\"type\":\"progress\",\"phase\":\"worker.python\",\"fraction\":0.5}}")
        sys.exit(0)

if __name__ == "__main__":
    main()

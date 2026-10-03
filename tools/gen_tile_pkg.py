#!/usr/bin/env python3
import argparse
import pathlib
import sys


def emit_byte_array(out, name, data):
    out.write(f"const unsigned char {name}[] = {{\n")
    for i in range(0, len(data), 20):
        chunk = data[i : i + 20]
        out.write("  " + ",".join(str(b) for b in chunk) + ",\n")
    out.write("};\n")


def main():
    parser = argparse.ArgumentParser(description="Embed a PKG as a C byte array.")
    parser.add_argument("--pkg", required=True)
    parser.add_argument("--out", required=True)
    args = parser.parse_args()

    pkg_path = pathlib.Path(args.pkg)
    if not pkg_path.exists():
        sys.exit(f"error: {pkg_path} not found")

    data = pkg_path.read_bytes()
    out_path = pathlib.Path(args.out)
    out_path.parent.mkdir(parents=True, exist_ok=True)

    with out_path.open("w") as out:
        emit_byte_array(out, "kTilePkg", data)
        out.write(f"const unsigned int kTilePkgSize = {len(data)}u;\n")
        out.write(f"const unsigned int kTilePkgRawSize = {len(data)}u;\n")

    print(f"wrote {out_path}: {len(data)} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())

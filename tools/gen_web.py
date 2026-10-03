#!/usr/bin/env python3
import argparse
import os
import sys


def emit(f, name, data):
    f.write("const unsigned char %s[] = {\n" % name)
    for i in range(0, len(data), 16):
        chunk = data[i : i + 16]
        f.write("  " + ", ".join("0x%02x" % b for b in chunk) + ",\n")
    f.write("};\n")
    f.write("const unsigned int %sLen = %d;\n\n" % (name, len(data)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--html", required=True)
    ap.add_argument("--qrcode", required=True)
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    html = open(args.html, "rb").read()
    qr = open(args.qrcode, "rb").read()
    if b"\x00" in html:
        sys.exit("index.html contains NUL bytes")

    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    with open(args.out, "w") as f:
        f.write("#include <stddef.h>\n\n")
        emit(f, "kIndexHtml", html)
        emit(f, "kQrcodeJs", qr)


if __name__ == "__main__":
    main()

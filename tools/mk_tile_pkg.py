#!/usr/bin/env python3
import argparse
import ctypes
import pathlib
import sys

LPP_ABI_VERSION = 7
LPP_INNER_NWONLY_DATA_FIRST = 3
LPP_INNER_NONE = 0


class LppLaunchReadiness(ctypes.Structure):
    _fields_ = [
        ("struct_size", ctypes.c_int32),
        ("has_eboot", ctypes.c_int32),
        ("has_param_json", ctypes.c_int32),
        ("has_param_sfo", ctypes.c_int32),
        ("requires_debug_console", ctypes.c_int32),
        ("is_launch_ready", ctypes.c_int32),
        ("module_count", ctypes.c_int32),
        ("issue_count", ctypes.c_int32),
    ]


def load_lib(path):
    if not pathlib.Path(path).exists():
        sys.exit(f"error: {path} not found (set --lib or LPP_LIB)")
    lib = ctypes.CDLL(path)
    lib.lpp_package_homebrew.argtypes = [
        ctypes.c_char_p,
        ctypes.c_char_p,
        ctypes.c_char_p,
        ctypes.c_char_p,
        ctypes.c_char_p,
        ctypes.c_char_p,
        ctypes.c_char_p,
        ctypes.c_int,
        ctypes.POINTER(LppLaunchReadiness),
        ctypes.c_char_p,
        ctypes.c_int,
    ]
    lib.lpp_package_homebrew.restype = ctypes.c_int
    lib.lpp_last_error.argtypes = [ctypes.c_char_p, ctypes.c_int]
    lib.lpp_last_error.restype = ctypes.c_int
    lib.lpp_launch_readiness_issues.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int]
    lib.lpp_launch_readiness_issues.restype = ctypes.c_int
    lib.lpp_version.argtypes = []
    lib.lpp_version.restype = ctypes.c_char_p
    lib.lpp_abi_version.argtypes = []
    lib.lpp_abi_version.restype = ctypes.c_int
    lib.lpp_keys_available.argtypes = []
    lib.lpp_keys_available.restype = ctypes.c_int
    lib.lpp_is_valid_content_id.argtypes = [ctypes.c_char_p]
    lib.lpp_is_valid_content_id.restype = ctypes.c_int
    lib.lpp_is_valid_title_id.argtypes = [ctypes.c_char_p]
    lib.lpp_is_valid_title_id.restype = ctypes.c_int
    return lib


def last_error(lib):
    buf = ctypes.create_string_buffer(1024)
    lib.lpp_last_error(buf, len(buf))
    return buf.value.decode("utf-8", "replace")


def main():
    parser = argparse.ArgumentParser(description="Build a debug PKG from a homebrew folder.")
    parser.add_argument("--lib", default="tools/lib/libprosperopkg.so")
    parser.add_argument("--homebrew", required=True)
    parser.add_argument("--out-pkg", required=True)
    parser.add_argument("--content-id", required=True)
    parser.add_argument("--title", default="")
    parser.add_argument("--version", default="")
    parser.add_argument("--passcode", default=None)
    parser.add_argument("--module", default=None)
    parser.add_argument("--inner-compression", type=int, default=LPP_INNER_NWONLY_DATA_FIRST)
    args = parser.parse_args()

    lib = load_lib(args.lib)

    abi = lib.lpp_abi_version()
    print(f"libprosperopkg {lib.lpp_version().decode()} abi {abi} keys {lib.lpp_keys_available()}")

    if not lib.lpp_is_valid_content_id(args.content_id.encode()):
        sys.exit(f"error: invalid content id {args.content_id}")

    title_id = args.content_id[7:16]
    if not lib.lpp_is_valid_title_id(title_id.encode()):
        print(f"note: {title_id} is not a PPSA id, param.json titleId must carry it")

    homebrew = pathlib.Path(args.homebrew).resolve()
    out_pkg = pathlib.Path(args.out_pkg).resolve()
    out_pkg.parent.mkdir(parents=True, exist_ok=True)
    out_dir = out_pkg.parent

    readiness = LppLaunchReadiness()
    readiness.struct_size = ctypes.sizeof(LppLaunchReadiness)
    out_path = ctypes.create_string_buffer(4096)

    rc = lib.lpp_package_homebrew(
        str(homebrew).encode(),
        str(out_dir).encode(),
        args.content_id.encode(),
        (args.passcode or "").encode(),
        args.title.encode(),
        args.version.encode(),
        (args.module or "").encode(),
        args.inner_compression,
        ctypes.byref(readiness),
        out_path,
        len(out_path),
    )

    if rc != 0:
        sys.exit(f"error: lpp_package_homebrew failed: {last_error(lib)}")

    produced = pathlib.Path(out_path.value.decode())
    if produced != out_pkg:
        out_pkg.write_bytes(produced.read_bytes())
        produced.unlink()

    issues = ctypes.create_string_buffer(4096)
    lib.lpp_launch_readiness_issues(str(homebrew).encode(), issues, len(issues))
    print(
        f"eboot={readiness.has_eboot} param={readiness.has_param_json} "
        f"sfo={readiness.has_param_sfo} modules={readiness.module_count} "
        f"issues={readiness.issue_count} debug_only={readiness.requires_debug_console}"
    )
    if issues.value.strip():
        print(issues.value.decode("utf-8", "replace"))
    print(f"wrote {out_pkg}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path


def run(cmd: list[str], cwd: Path | None = None, env: dict[str, str] | None = None) -> None:
    print("+", " ".join(cmd))
    subprocess.run(cmd, cwd=cwd, env=env, check=True)


def run_capture(
    cmd: list[str], cwd: Path | None = None, env: dict[str, str] | None = None
) -> tuple[int, str]:
    print("+", " ".join(cmd))
    proc = subprocess.run(
        cmd,
        cwd=cwd,
        env=env,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    output = proc.stdout or ""
    if output:
        print(output, end="" if output.endswith("\n") else "\n")
    return proc.returncode, output


def parse_cache_var(cache_path: Path, var_name: str) -> str | None:
    pattern = re.compile(rf"^{re.escape(var_name)}:[^=]*=(.*)$")
    for line in cache_path.read_text(encoding="utf-8", errors="ignore").splitlines():
        match = pattern.match(line.strip())
        if match:
            return match.group(1).strip()
    return None


def ensure_tool(tool: str) -> None:
    if shutil.which(tool):
        return
    raise RuntimeError(f"Required tool not found in PATH: {tool}")


def append_gst_plugin_path(paths: list[Path]) -> None:
    env_file = os.environ.get("GITHUB_ENV")
    if not env_file:
        return
    existing = os.environ.get("GST_PLUGIN_PATH", "")
    merged = os.pathsep.join(str(p) for p in paths if p)
    if existing:
        merged = f"{merged}{os.pathsep}{existing}" if merged else existing
    with open(env_file, "a", encoding="utf-8") as fh:
        fh.write(f"GST_PLUGIN_PATH={merged}\n")


def install_plugin_binary(src: Path, dst_dir: Path) -> None:
    dst_dir.mkdir(parents=True, exist_ok=True)
    dst = dst_dir / src.name
    try:
        shutil.copy2(src, dst)
        return
    except PermissionError:
        pass

    if shutil.which("sudo"):
        run(["sudo", "cp", str(src), str(dst)])
        return
    raise RuntimeError(f"Permission denied copying {src} to {dst_dir}")


def is_gstreamer_compat_failure(output: str) -> bool:
    markers = [
        "required version is '>= 1.22'",
        "The system library `gstreamer-1.0` required by crate `gstreamer-sys` was not found.",
        "Package dependency requirement 'gstreamer-1.0 >= 1.22' could not be satisfied.",
        "pkg-config exited with status code 1",
    ]
    return any(m in output for m in markers)


def build_linux(
    work_dir: Path, env: dict[str, str], gst_root: Path, refs: list[str]
) -> tuple[Path, str]:
    ensure_tool("cargo")
    ensure_tool("pkg-config")
    env = env.copy()
    pkg_paths = [
        gst_root / "lib" / "pkgconfig",
        gst_root / "lib64" / "pkgconfig",
        gst_root / "lib" / "x86_64-linux-gnu" / "pkgconfig",
        gst_root / "lib" / "aarch64-linux-gnu" / "pkgconfig",
        gst_root / "lib" / "gstreamer-1.0" / "pkgconfig",
    ]
    existing_pkg_path = env.get("PKG_CONFIG_PATH", "")
    pc_path_rc, pc_path_out = run_capture(["pkg-config", "--variable=pc_path", "pkg-config"], env=env)
    if pc_path_rc == 0 and pc_path_out.strip():
        for entry in pc_path_out.strip().split(":"):
            if entry:
                pkg_paths.append(Path(entry))
    merged_pkg_path = os.pathsep.join(str(p) for p in pkg_paths if p.exists())
    if existing_pkg_path:
        merged_pkg_path = (
            f"{merged_pkg_path}{os.pathsep}{existing_pkg_path}"
            if merged_pkg_path
            else existing_pkg_path
        )
    if merged_pkg_path:
        env["PKG_CONFIG_PATH"] = merged_pkg_path
    env["PKG_CONFIG_ALLOW_SYSTEM_CFLAGS"] = "1"
    print(f"Linux PKG_CONFIG_PATH={env.get('PKG_CONFIG_PATH', '')}")
    exists_rc, _ = run_capture(["pkg-config", "--exists", "gstreamer-1.0"], env=env)
    if exists_rc != 0 and shutil.which("sudo"):
        print("gstreamer-1.0.pc not visible to pkg-config; installing Linux GStreamer dev packages.")
        run(["sudo", "apt-get", "-o", "Acquire::Retries=3", "update", "-qq"])
        run(
            [
                "sudo",
                "apt-get",
                "-o",
                "Acquire::Retries=3",
                "install",
                "-y",
                "--no-install-recommends",
                "libgstreamer1.0-dev",
                "libgstreamer-plugins-base1.0-dev",
            ]
        )
        exists_rc, _ = run_capture(["pkg-config", "--exists", "gstreamer-1.0"], env=env)
        if exists_rc != 0:
            raise RuntimeError("gstreamer-1.0.pc is still not visible after installing dev packages.")
    _modver_rc, _modver_out = run_capture(["pkg-config", "--modversion", "gstreamer-1.0"], env=env)
    if _modver_rc != 0:
        print("Warning: unable to query gstreamer-1.0 version via pkg-config before build.")

    run(["cargo", "install", "--locked", "cargo-c"], env=env)
    plugin_dir = gst_root / "lib" / "gstreamer-1.0"
    if not plugin_dir.exists():
        plugin_dir = Path(env.get("RUNNER_TEMP", "/tmp")) / "rswebrtc-plugins"
        plugin_dir.mkdir(parents=True, exist_ok=True)
    last_compat_error = ""
    for ref in refs:
        print(f"Attempting Linux rswebrtc build with gst-plugins-rs ref: {ref}")
        code, output = run_capture(
            [
                "cargo",
                "cbuild",
                "-p",
                "gst-plugin-webrtc",
                "--release",
            ],
            cwd=work_dir,
            env=env,
        )
        if code == 0:
            src = work_dir / "target" / "release" / "libgstrswebrtc.so"
            if not src.exists():
                raise RuntimeError(f"Built plugin not found: {src}")
            install_plugin_binary(src, plugin_dir)
            return plugin_dir, ref

        if is_gstreamer_compat_failure(output):
            last_compat_error = output
            print(f"Compatibility failure with ref {ref}; trying older ref.")
            continue

        raise RuntimeError(
            f"Linux rswebrtc build failed on ref {ref} for a non-compatibility reason."
        )

    raise RuntimeError(
        "Linux rswebrtc build failed for all compatibility refs "
        f"{refs}. Last compatibility error snippet:\n{last_compat_error[:1200]}"
    )


def build_windows(work_dir: Path, env: dict[str, str], gst_root: Path, arch: str) -> Path:
    ensure_tool("cargo")
    target = "x86_64-pc-windows-msvc" if arch == "x86_64" else "aarch64-pc-windows-msvc"
    run(["rustup", "target", "add", target], env=env)
    run(["cargo", "install", "--locked", "cargo-c"], env=env)
    pkg_config = gst_root / "bin" / "pkg-config.exe"
    if not pkg_config.exists():
        raise RuntimeError(f"pkg-config.exe not found: {pkg_config}")
    env = env.copy()
    env["PKG_CONFIG"] = str(pkg_config)
    pkg_paths = [
        gst_root / "lib" / "pkgconfig",
        gst_root / "lib" / "gstreamer-1.0" / "pkgconfig",
    ]
    env["PKG_CONFIG_PATH"] = os.pathsep.join(str(p) for p in pkg_paths if p.exists())
    plugin_dir = gst_root / "lib" / "gstreamer-1.0"
    plugin_dir.mkdir(parents=True, exist_ok=True)
    run(
        [
            "cargo",
            "cbuild",
            "-p",
            "gst-plugin-webrtc",
            "--release",
            "--target",
            target,
        ],
        cwd=work_dir,
        env=env,
    )
    src = work_dir / "target" / target / "release" / "gstrswebrtc.dll"
    if not src.exists():
        raise RuntimeError(f"Built plugin not found: {src}")
    install_plugin_binary(src, plugin_dir)
    return plugin_dir


def build_android(work_dir: Path, env: dict[str, str], gst_source_dir: Path, abis: list[str]) -> list[Path]:
    ensure_tool("cargo")
    ndk_root = env.get("ANDROID_NDK_ROOT") or env.get("ANDROID_NDK_HOME")
    if not ndk_root:
        raise RuntimeError("ANDROID_NDK_ROOT/ANDROID_NDK_HOME is not set")
    toolchain = (
        Path(ndk_root) / "toolchains" / "llvm" / "prebuilt" / "linux-x86_64" / "bin"
    )
    if not toolchain.exists():
        raise RuntimeError(f"NDK toolchain path not found: {toolchain}")

    abi_map = {
        "arm64-v8a": ("aarch64-linux-android", "arm64", "aarch64-linux-android21-clang"),
        "armeabi-v7a": (
            "armv7-linux-androideabi",
            "armv7",
            "armv7a-linux-androideabi21-clang",
        ),
        "x86_64": ("x86_64-linux-android", "x86_64", "x86_64-linux-android21-clang"),
        "x86": ("i686-linux-android", "x86", "i686-linux-android21-clang"),
    }
    unsupported = [abi for abi in abis if abi not in abi_map]
    if unsupported:
        supported = ", ".join(sorted(abi_map.keys()))
        raise RuntimeError(
            f"Unsupported Android ABI(s): {unsupported}. Supported ABIs: {supported}"
        )

    run(["cargo", "install", "--locked", "cargo-c"], env=env)
    copied_dirs: list[Path] = []
    print(f"Android NDK toolchain dir: {toolchain}")
    print(f"Android requested ABIs: {abis}")
    for abi in abis:
        target, gst_abi_dir, clang = abi_map[abi]
        gst_root = gst_source_dir / gst_abi_dir
        plugin_dir = gst_root / "lib" / "gstreamer-1.0"
        plugin_dir.mkdir(parents=True, exist_ok=True)
        run(["rustup", "target", "add", target], env=env)
        build_env = env.copy()
        build_env["PKG_CONFIG_ALLOW_CROSS"] = "1"
        build_env["PKG_CONFIG_PATH"] = os.pathsep.join(
            str(p)
            for p in [
                gst_root / "lib" / "pkgconfig",
                gst_root / "lib" / "gstreamer-1.0" / "pkgconfig",
            ]
            if p.exists()
        )
        cc = str(toolchain / clang)
        ar = str(toolchain / "llvm-ar")
        ranlib = str(toolchain / "llvm-ranlib")
        upper = target.upper().replace("-", "_")
        build_env["CC"] = cc
        build_env["AR"] = ar
        build_env["RANLIB"] = ranlib
        build_env[f"CC_{target}"] = cc
        build_env[f"CXX_{target}"] = cc
        build_env[f"AR_{target}"] = ar
        build_env[f"RANLIB_{target}"] = ranlib
        build_env[f"CARGO_TARGET_{upper}_LINKER"] = cc
        print(f"Android target {target}: CC={cc}")
        print(f"Android target {target}: AR={ar}")
        print(f"Android target {target}: RANLIB={ranlib}")
        run(
            [
                "cargo",
                "cbuild",
                "-p",
                "gst-plugin-webrtc",
                "--release",
                "--target",
                target,
            ],
            cwd=work_dir,
            env=build_env,
        )
        src = work_dir / "target" / target / "release" / "libgstrswebrtc.so"
        if not src.exists():
            raise RuntimeError(f"Built plugin not found: {src}")
        install_plugin_binary(src, plugin_dir)
        copied_dirs.append(plugin_dir)
    return copied_dirs


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--platform", choices=["linux", "windows", "android"], required=True)
    parser.add_argument("--build-dir", required=True)
    parser.add_argument("--gst-plugins-rs-ref", default="main")
    parser.add_argument(
        "--linux-fallback-refs",
        default="0.14,0.13,0.12,0.11,0.10,0.9",
        help="Comma-separated ref list for Linux compatibility fallback",
    )
    parser.add_argument("--windows-arch", choices=["x86_64", "arm64"], default="x86_64")
    args = parser.parse_args()

    build_dir = Path(args.build_dir).resolve()
    cache_file = build_dir / "CMakeCache.txt"
    if not cache_file.exists():
        raise RuntimeError(f"CMake cache not found: {cache_file}")

    runner_temp = Path(os.environ.get("RUNNER_TEMP", "/tmp"))
    source_dir = runner_temp / "gst-plugins-rs"
    env = os.environ.copy()

    if args.platform == "android":
        if source_dir.exists():
            shutil.rmtree(source_dir)
        run(
            [
                "git",
                "clone",
                "--depth",
                "1",
                "--branch",
                args.gst_plugins_rs_ref,
                "https://gitlab.freedesktop.org/gstreamer/gst-plugins-rs.git",
                str(source_dir),
            ]
        )
        gstreamer_src = parse_cache_var(cache_file, "gstreamer_SOURCE_DIR")
        if not gstreamer_src:
            gst_root = parse_cache_var(cache_file, "GStreamer_ROOT_DIR")
            if not gst_root:
                raise RuntimeError(
                    "Neither gstreamer_SOURCE_DIR nor GStreamer_ROOT_DIR found in CMake cache"
                )
            gst_root_path = Path(gst_root)
            # Android roots are typically <universal-sdk>/<abi-dir>, derive universal root.
            if gst_root_path.name in {"arm64", "armv7", "x86", "x86_64"}:
                gstreamer_src = str(gst_root_path.parent)
            else:
                gstreamer_src = str(gst_root_path)
        qt_abis = parse_cache_var(cache_file, "QT_ANDROID_ABIS") or "arm64-v8a;armeabi-v7a"
        plugin_dirs = build_android(source_dir, env, Path(gstreamer_src), qt_abis.split(";"))
        print("Built rswebrtc for Android plugin dirs:")
        for d in plugin_dirs:
            print(f"  - {d}")
        append_gst_plugin_path(plugin_dirs)
        return 0

    gst_root = parse_cache_var(cache_file, "GStreamer_ROOT_DIR")
    if not gst_root:
        raise RuntimeError("GStreamer_ROOT_DIR not found in CMake cache")
    gst_root_path = Path(gst_root)

    if args.platform == "linux":
        refs = [r.strip() for r in args.linux_fallback_refs.split(",") if r.strip()]
        if args.gst_plugins_rs_ref and args.gst_plugins_rs_ref not in refs:
            refs.insert(0, args.gst_plugins_rs_ref)
        plugin_dir = None
        used_ref = ""
        for idx, ref in enumerate(refs):
            if source_dir.exists():
                shutil.rmtree(source_dir)
            run(
                [
                    "git",
                    "clone",
                    "--depth",
                    "1",
                    "--branch",
                    ref,
                    "https://gitlab.freedesktop.org/gstreamer/gst-plugins-rs.git",
                    str(source_dir),
                ]
            )
            try:
                plugin_dir, used_ref = build_linux(source_dir, env, gst_root_path, [ref])
                break
            except Exception as exc:  # noqa: BLE001
                msg = str(exc)
                if "non-compatibility reason" in msg:
                    raise
                if idx == len(refs) - 1:
                    raise
                print(f"Linux fallback moving to next ref after {ref}: {exc}")
        assert plugin_dir is not None
        print(f"Linux rswebrtc selected ref: {used_ref}")
        print(f"Built rswebrtc plugin dir: {plugin_dir}")
        append_gst_plugin_path([plugin_dir])
        return 0

    if source_dir.exists():
        shutil.rmtree(source_dir)
    run(
        [
            "git",
            "clone",
            "--depth",
            "1",
            "--branch",
            args.gst_plugins_rs_ref,
            "https://gitlab.freedesktop.org/gstreamer/gst-plugins-rs.git",
            str(source_dir),
        ]
    )

    plugin_dir = build_windows(source_dir, env, gst_root_path, args.windows_arch)
    print(f"Built rswebrtc plugin dir: {plugin_dir}")
    append_gst_plugin_path([plugin_dir])
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:  # noqa: BLE001
        print(f"ERROR: {exc}", file=sys.stderr)
        raise SystemExit(1)

#!/usr/bin/env python3
"""Copy non-system Mach-O dependencies into a flat, relocatable release folder.

Usage: bundle_macos.sh <dir-with-binaries> <binary> [binary...]
Only build tools use Python; the portable release does not need it.
"""

import argparse
from dataclasses import dataclass
from pathlib import Path
import re
import shutil
import subprocess


LOAD_COMMANDS = {
    "LC_LOAD_DYLIB", "LC_LOAD_WEAK_DYLIB", "LC_REEXPORT_DYLIB",
    "LC_LOAD_UPWARD_DYLIB", "LC_LAZY_LOAD_DYLIB",
}


@dataclass
class MachO:
    dependencies: list
    rpaths: list
    install_id: str = ""


def inspect(path):
    output = subprocess.check_output(["otool", "-l", str(path)], text=True)
    info = MachO([], [])
    command = ""
    for line in output.splitlines():
        line = line.strip()
        if line.startswith("cmd "):
            command = line[4:]
        elif line.startswith(("name ", "path ")):
            # Unlike splitting on whitespace, this preserves spaces in paths.
            value = re.sub(r" \(offset \d+\)$", "", line.split(" ", 1)[1])
            if command in LOAD_COMMANDS and value not in info.dependencies:
                info.dependencies.append(value)
            elif command == "LC_RPATH" and value not in info.rpaths:
                info.rpaths.append(value)
            elif command == "LC_ID_DYLIB":
                info.install_id = value
    return info


def is_system(path):
    return path.startswith(("/usr/lib/", "/System/"))


def expand(path, loader_dir, executable_dir):
    for token, base in (("@loader_path", loader_dir),
                        ("@executable_path", executable_dir)):
        if path == token or path.startswith(token + "/"):
            return base / path[len(token):].lstrip("/")
    if path.startswith("/"):
        return Path(path)
    raise RuntimeError(f"unsupported library search path: {path}")


def bundle(out, binaries):
    out = out.resolve(strict=True)
    staged = {}

    def visit(source, destination, executable_dir, inherited_rpaths):
        if destination in staged:
            return
        if destination.is_symlink():
            raise RuntimeError(f"staged library must not be a symlink: {destination}")
        # Keep the original location until the entire graph has been copied:
        # @loader_path in a copied dylib refers to its original siblings here.
        original = inspect(source)
        if source != destination and not destination.exists():
            shutil.copy2(source, destination)
            destination.chmod(destination.stat().st_mode | 0o200)
            print(f"bundled {destination.name}")
        info = inspect(destination) if source != destination else original
        staged[destination] = info  # Mark before visiting cycles.
        rpaths = [expand(p, source.parent, executable_dir) for p in original.rpaths]
        rpaths.extend(inherited_rpaths)

        for dependency in info.dependencies:
            if is_system(dependency):
                continue
            if dependency.startswith("@rpath/"):
                candidates = [p / dependency[len("@rpath/"):] for p in rpaths]
            else:
                candidates = [expand(dependency, source.parent, executable_dir)]
            # Earlier packaging steps may already have staged a dependency,
            # with the original builder's paths no longer available.
            target = out / Path(dependency).name
            candidates.append(target)
            resolved = next((p.resolve() for p in candidates if p.is_file()), None)
            if resolved is None:
                searched = "\n  ".join(str(p) for p in candidates)
                raise RuntimeError(
                    f"{destination.name}: cannot resolve {dependency}; tried:\n  {searched}")
            visit(resolved, target, executable_dir, rpaths)

    # SDK GPU plugins and pre-staged runtimes may only be loaded with dlopen,
    # so start from them as well as the explicitly supplied executables.
    roots = list(dict.fromkeys([*binaries, *sorted(out.glob("*.dylib"))]))
    for binary in roots:
        binary = binary.resolve(strict=True)
        if binary.parent != out:
            raise RuntimeError(f"binary must be staged directly in {out}: {binary}")
        visit(binary, binary, binary.parent, [])

    # Resolve first, then mutate: rewriting LC_RPATH earlier destroys the only
    # route to dependencies such as JPEG XL's @rpath-relative CMS library.
    for binary, info in staged.items():
        changes = []
        for dependency in info.dependencies:
            if not is_system(dependency):
                replacement = "@loader_path/" + Path(dependency).name
                if dependency != replacement:
                    changes.extend(["-change", dependency, replacement])
        if info.install_id:
            changes.extend(["-id", "@loader_path/" + binary.name])
        for rpath in info.rpaths:
            if rpath != "@loader_path":
                changes.extend(["-delete_rpath", rpath])
        if "@loader_path" not in info.rpaths:
            changes.extend(["-add_rpath", "@loader_path"])
        if changes:
            binary.chmod(binary.stat().st_mode | 0o200)
            subprocess.run(["install_name_tool", *changes, str(binary)], check=True)

    # Check the mutated files, not the source graph. A missing relative import
    # used to pass the Homebrew-absolute-path grep in package_release.sh.
    for binary in staged:
        info = inspect(binary)
        for dependency in info.dependencies:
            if is_system(dependency):
                continue
            name = Path(dependency).name
            if (dependency != "@loader_path/" + name
                    or not (out / name).is_file()
                    or (out / name).resolve().parent != out):
                raise RuntimeError(f"{binary.name}: dependency escapes bundle: {dependency}")
        if any(p != "@loader_path" for p in info.rpaths):
            raise RuntimeError(f"{binary.name}: library search path escapes bundle")
    print(f"verified {len(staged)} bundled Mach-O files")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("out", type=Path)
    parser.add_argument("binaries", nargs="+", type=Path)
    args = parser.parse_args()
    try:
        bundle(args.out, args.binaries)
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"macOS bundling failed: {error}\n")


if __name__ == "__main__":
    main()

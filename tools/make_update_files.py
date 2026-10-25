#!/usr/bin/env python3
"""Release files for the TRG Launcher updater (include/trg/updater.h).

  manifest <folder> --version V   writes the file list and the retired list into the program folder
  package  <folder> --version V --zip OUT.zip
                                  the same, then the release zip (the folder inside it) and SHA256SUMS.txt
  check    OUT.zip                checks a release zip and the files beside it; exit code 1 on any problem

The file list (update-files.txt) names every file of the release except itself, one "<sha256> <size> <path>" line
each, plus a "version V" line. The retired list (update-retired.txt) names the files earlier releases shipped that
this one does not: pass the earlier release zips (or their file lists) with --earlier. An earlier zip's own retired
list counts too, so passing the previous release is enough once every release was made with this script. The
updater removes a retired file only while it is unchanged.

Examples:
  python tools/make_update_files.py package out/MyGame --version 1.2.0 --earlier releases/MyGame-v1.1.0-windows-x64.zip
      --protect game --protect settings.toml --zip out/MyGame-v1.2.0-windows-x64.zip
  python tools/make_update_files.py check out/MyGame-v1.2.0-windows-x64.zip --version 1.2.0 --protect game

Standard library only.
"""
import argparse
import fnmatch
import hashlib
import os
import re
import sys
import zipfile

LINE = re.compile(r"^([0-9a-fA-F]{64}) (\d+) (.+)$")


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def safe_path(p):
    if not p or len(p) > 1024 or p.startswith("/") or any(c in p for c in "\\:") or any(ord(c) < 0x20 for c in p):
        return False
    return all(part not in ("", ".", "..") for part in p.split("/"))


def is_protected(path, protect):
    p = path.lower()
    for e in protect:
        e = e.lower().replace("\\", "/").strip("/")
        if e and (p == e or p.startswith(e + "/")):
            return True
    return False


def parse_list(text, name, problems=None):
    """{path: (sha256, size)} and the version of a file list or retired list."""
    files, version = {}, ""
    for n, line in enumerate(text.lstrip("﻿").splitlines(), 1):
        if not line or line.startswith("#"):
            continue
        if line.startswith("version "):
            version = line[8:]
            continue
        m = LINE.match(line)
        if not m or not safe_path(m.group(3)):
            msg = f"{name} line {n}: broken or unsafe: {line}"
            if problems is None:
                raise SystemExit(msg)
            problems.append(msg)
            continue
        files.setdefault(m.group(3), []).append((m.group(1).lower(), int(m.group(2))))
    return files, version


def zip_tree(path):
    """{relative path: zip entry} below the zip's one top folder, and that folder's name."""
    with zipfile.ZipFile(path) as z:
        entries = [e for e in z.infolist() if not e.is_dir()]
    tops = {e.filename.split("/", 1)[0] for e in entries}
    if len(tops) == 1 and all("/" in e.filename for e in entries):
        top = tops.pop()
        return {e.filename[len(top) + 1:]: e for e in entries}, top
    return {e.filename: e for e in entries}, ""


def earlier_files(path, args):
    """[(path, sha256, size)] an earlier release shipped (and those its retired list names)."""
    out = []
    if path.lower().endswith(".zip"):
        tree, top = zip_tree(path)
        with zipfile.ZipFile(path) as z:
            for rel, e in tree.items():
                data = z.read(e)
                out.append((rel, hashlib.sha256(data).hexdigest(), len(data)))
                if rel == args.retired_list:
                    for p, versions in parse_list(data.decode("utf-8"), path + ":" + rel)[0].items():
                        out += [(p, s, n) for s, n in versions]
    else:
        with open(path, encoding="utf-8") as f:
            for p, versions in parse_list(f.read(), path)[0].items():
                out += [(p, s, n) for s, n in versions]
    return out


def write_text(path, text):
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)


def make_manifest(args):
    root = os.path.abspath(args.folder)
    lists = {args.files_list.lower(), args.retired_list.lower()}
    files = {}
    for d, _, names in os.walk(root):
        for name in names:
            full = os.path.join(d, name)
            rel = os.path.relpath(full, root).replace(os.sep, "/")
            if rel.lower() in lists:
                continue
            if not safe_path(rel):
                raise SystemExit(f"unsafe file name in the package: {rel}")
            if is_protected(rel, args.protect):
                raise SystemExit(f"a protected path is in the package: {rel}")
            if rel.lower().endswith((".old", ".new", ".part")):
                raise SystemExit(f"a leftover file is in the package: {rel}")
            files[rel] = full
    current = {p.lower() for p in files}

    retired = {}
    for path in args.earlier or []:
        for p, sha, size in earlier_files(path, args):
            if p.lower() in current or p.lower() in lists or is_protected(p, args.protect):
                continue
            retired[(p.lower(), sha)] = f"{sha} {size} {p}"
    text = (f"# Files earlier releases shipped that version {args.version} does not (sha256, size, path).\n"
            "# The updater removes one only while it is unchanged. Made by make_update_files.py.\n"
            f"version {args.version}\n" + "".join(v + "\n" for _, v in sorted(retired.items())))
    write_text(os.path.join(root, args.retired_list), text)
    files[args.retired_list] = os.path.join(root, args.retired_list)

    lines = [f"{sha256_file(files[p])} {os.path.getsize(files[p])} {p}" for p in sorted(files)]
    text = (f"# The files of version {args.version} (sha256, size, path): every file of the release but this one.\n"
            "# The updater installs these and checks them on the first start. Made by make_update_files.py.\n"
            f"version {args.version}\n" + "".join(line + "\n" for line in lines))
    write_text(os.path.join(root, args.files_list), text)
    print(f"{args.files_list}: {len(lines)} files, version {args.version}")
    print(f"{args.retired_list}: {len(retired)} entries")
    return root


def write_sums(folder, paths):
    sums_path = os.path.join(folder, "SHA256SUMS.txt")
    lines = {}
    if os.path.exists(sums_path):
        with open(sums_path, encoding="utf-8") as f:
            for line in f.read().splitlines():
                if len(line) > 66:
                    lines[line[66:].lstrip("*")] = line
    for p in paths:
        lines[os.path.basename(p)] = f"{sha256_file(p)}  {os.path.basename(p)}"
    write_text(sums_path, "".join(lines[k] + "\n" for k in sorted(lines)))
    print(f"SHA256SUMS.txt: {', '.join(sorted(lines))}")


def package(args):
    root = make_manifest(args)
    top = args.folder_name or os.path.basename(root)
    os.makedirs(os.path.dirname(os.path.abspath(args.zip)), exist_ok=True)
    with zipfile.ZipFile(args.zip, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for d, _, names in sorted(os.walk(root)):
            for name in sorted(names):
                full = os.path.join(d, name)
                z.write(full, top + "/" + os.path.relpath(full, root).replace(os.sep, "/"))
    print(f"{args.zip}: {os.path.getsize(args.zip)} bytes")
    write_sums(os.path.dirname(os.path.abspath(args.zip)), [args.zip] + (args.also or []))
    return check(argparse.Namespace(**{**vars(args), "zip_path": args.zip}))


def check(args):
    problems, notes = [], []
    path = args.zip_path
    tree, top = zip_tree(path)
    if not top:
        problems.append("the zip has no single top folder (Game/...)")
    hashes = {}
    with zipfile.ZipFile(path) as z:
        for rel, e in tree.items():
            data = z.read(e)
            hashes[rel] = (hashlib.sha256(data).hexdigest(), len(data), data)
    listed = hashes.get(args.files_list)
    if listed is None:
        problems.append(f"{args.files_list} is missing from the zip's top folder")
    else:
        files, version = parse_list(listed[2].decode("utf-8"), args.files_list, problems)
        if args.version and version != args.version:
            problems.append(f"{args.files_list} says version '{version}', expected '{args.version}'")
        for rel, (sha, size, _) in hashes.items():
            if rel == args.files_list:
                continue
            if not safe_path(rel):
                problems.append(f"unsafe path in the zip: {rel}")
            want = files.get(rel)
            if not want:
                problems.append(f"in the zip but not in {args.files_list}: {rel}")
            elif (sha, size) != want[0]:
                problems.append(f"{rel}: the zip's copy differs from {args.files_list}")
        for rel in files:
            if rel not in hashes:
                problems.append(f"in {args.files_list} but not in the zip: {rel}")
        retired = hashes.get(args.retired_list)
        if retired:
            for rel in parse_list(retired[2].decode("utf-8"), args.retired_list, problems)[0]:
                if rel.lower() in {p.lower() for p in files}:
                    problems.append(f"{args.retired_list} names a file the release ships: {rel}")
    for rel in hashes:
        if is_protected(rel, args.protect):
            problems.append(f"a protected path is in the zip: {rel}")
        if rel.lower().endswith((".old", ".new", ".part")):
            problems.append(f"a leftover file is in the zip: {rel}")

    folder = os.path.dirname(os.path.abspath(path))
    name = os.path.basename(path)
    if args.asset and not fnmatch.fnmatchcase(name, args.asset):
        problems.append(f"{name} does not match the asset pattern {args.asset}")
    for other in sorted(os.listdir(folder)):
        if other != name and args.asset and fnmatch.fnmatchcase(other, args.asset):
            problems.append(f"another file beside it also matches {args.asset}: {other} (one asset per pattern)")
    sums = os.path.join(folder, "SHA256SUMS.txt")
    if os.path.exists(sums):
        with open(sums, encoding="utf-8") as f:
            line = next((l for l in f.read().splitlines() if l[66:].lstrip("*") == name), None)
        if not line:
            problems.append(f"SHA256SUMS.txt has no line for {name}")
        elif line[:64].lower() != sha256_file(path):
            problems.append(f"SHA256SUMS.txt does not match {name}")
    else:
        notes.append("no SHA256SUMS.txt beside the zip: attach one to the release, or the updater checks only the size")

    for n in notes:
        print("note:", n)
    for p in problems:
        print("FAIL:", p)
    print("PACKAGE CHECK " + ("FAILED" if problems else "PASSED") + f" ({len(hashes)} files)")
    return 1 if problems else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="command", required=True)
    for name in ("manifest", "package", "check"):
        p = sub.add_parser(name)
        if name == "check":
            p.add_argument("zip_path", metavar="zip")
            p.add_argument("--version", default="")
        else:
            p.add_argument("folder", help="the program folder, laid out as players get it")
            p.add_argument("--version", required=True)
            p.add_argument("--earlier", action="append", help="an earlier release zip or file list (repeat)")
        if name == "package":
            p.add_argument("--zip", required=True, help="the release zip to write")
            p.add_argument("--folder-name", default="", help="the folder inside the zip (default: the folder's name)")
            p.add_argument("--also", action="append", help="another asset for SHA256SUMS.txt, an AppImage (repeat)")
        p.add_argument("--protect", action="append", default=[], help="a protected path, as in UpdaterConfig")
        p.add_argument("--files-list", default="update-files.txt")
        p.add_argument("--retired-list", default="update-retired.txt")
        p.add_argument("--asset", default="*windows-x64.zip", help="the updater's asset pattern ('' to skip)")
    args = ap.parse_args()
    if args.command == "manifest":
        make_manifest(args)
        return 0
    return package(args) if args.command == "package" else check(args)


if __name__ == "__main__":
    sys.exit(main())

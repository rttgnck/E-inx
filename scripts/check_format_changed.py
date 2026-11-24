#!/usr/bin/env python3
"""CI formatting check that only looks at E-inx's own changes.

Runs `git clang-format --diff <base>` (clang-format 21, the repo's .clang-format) over the lines changed since
<base>, then drops every hunk whose original text appears verbatim in an upstream Inx version merged in that
range (the second parent of each merge commit on the first-parent chain). Upstream's code is therefore never
reformatted here, which keeps later upstream merges free of whitespace conflicts, while new E-inx code still
has to be formatted.

Usage: check_format_changed.py <base-commit>     (exit 1 and print the diff if E-inx lines need formatting)
Fix locally with: git clang-format --binary clang-format-21 <base-commit>
"""
import os
import re
import shutil
import subprocess
import sys

EXTENSIONS = ["*.c", "*.cpp", "*.h", "*.hpp"]


def find_tool(names):
    for name in names:
        if os.path.isabs(name) and os.access(name, os.X_OK):
            return name
        path = shutil.which(name)
        if path:
            return path
    return None


def git(*args):
    return subprocess.run(["git", *args], capture_output=True, text=True, check=True).stdout


def upstream_versions(base, path, cache):
    """Contents of `path` in every merged-in (second-parent) commit between base and HEAD."""
    if "refs" not in cache:
        merges = git("rev-list", "--first-parent", "--merges", f"{base}..HEAD").split()
        cache["refs"] = [f"{m}^2" for m in merges]
    out = []
    for ref in cache["refs"]:
        key = (ref, path)
        if key not in cache:
            r = subprocess.run(["git", "show", f"{ref}:{path}"], capture_output=True, text=True)
            cache[key] = r.stdout if r.returncode == 0 else ""
        if cache[key]:
            out.append(cache[key])
    return out


def split_hunks(diff_text):
    """Yields (path, header_lines, hunk_lines) for each hunk of a unified diff."""
    path = None
    file_header = []
    hunk = None
    for line in diff_text.splitlines():
        if line.startswith("diff --git "):
            if hunk:
                yield path, file_header, hunk
            hunk = None
            m = re.match(r"diff --git a/(.+) b/(.+)$", line)
            path = m.group(2) if m else None
            file_header = [line]
        elif hunk is None and not line.startswith("@@"):
            file_header.append(line)
        elif line.startswith("@@"):
            if hunk:
                yield path, file_header, hunk
            hunk = [line]
        elif hunk is not None:
            hunk.append(line)
    if hunk:
        yield path, file_header, hunk


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    base = sys.argv[1]
    git_cf = find_tool(["git-clang-format-21", "/usr/lib/llvm-21/bin/git-clang-format", "git-clang-format"])
    cf = os.environ.get("CLANG_FORMAT") or find_tool(
        ["clang-format-21", "/usr/lib/llvm-21/bin/clang-format", "clang-format"])
    if not git_cf or not cf:
        sys.exit("check_format_changed: git-clang-format / clang-format 21 not found")

    cmd = [git_cf, "--binary", cf, "--diff", base, "--", *EXTENSIONS]
    if git_cf.endswith(".py") or open(git_cf, "rb").read(2) == b"#!":
        cmd = [sys.executable, *cmd]
    result = subprocess.run(cmd, capture_output=True, text=True)
    diff_text = result.stdout
    if not diff_text.startswith("diff --git"):
        print(diff_text.strip() or "clang-format: no changes")
        return 0

    cache = {}
    kept = []
    skipped = 0
    for path, header, hunk in split_hunks(diff_text):
        old_side = "\n".join(l[1:] for l in hunk[1:] if l[:1] in (" ", "-"))
        if old_side and any(old_side in text for text in upstream_versions(base, path, cache)):
            skipped += 1
            continue
        kept.append((path, header, hunk))

    if skipped:
        print(f"Ignored {skipped} hunk(s) in code merged unchanged from upstream Inx.")
    if not kept:
        print("Formatting OK for E-inx's changes.")
        return 0

    printed = set()
    for path, header, hunk in kept:
        if path not in printed:
            print("\n".join(header))
            printed.add(path)
        print("\n".join(hunk))
    print(f"\n{len(kept)} hunk(s) in E-inx's changes need clang-format. "
          f"Run: git clang-format --binary clang-format-21 {base}")
    return 1


if __name__ == "__main__":
    sys.exit(main())

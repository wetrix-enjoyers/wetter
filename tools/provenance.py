"""Compare source trees for shared code (provenance checks).

    python tools/provenance.py <tree> <corpus> [<corpus> ...]

Comments and whitespace are dropped; identifiers, numbers and punctuation are
kept. Every shared run of MIN_RUN or more tokens is printed with both places.
"""
import os
import re
import sys
from collections import defaultdict

N = 12          # shingle length (tokens)
MIN_RUN = 24    # report shared runs at least this long

TOKEN = re.compile(r"[A-Za-z_][A-Za-z_0-9]*|0[xX][0-9A-Fa-f]+|\d+\.?\d*[fFuUlL]*|\S")
COMMENT = re.compile(r"//[^\n]*|/\*.*?\*/", re.S)
EXT = (".cpp", ".cc", ".c", ".h", ".hpp", ".inl", ".hlsl", ".hlsli", ".glsl", ".frag", ".vert")


def tokens(path):
    try:
        src = open(path, encoding="utf-8", errors="ignore").read()
    except OSError:
        return []
    src = COMMENT.sub(" ", src)
    out = []
    for lineno, line in enumerate(src.split("\n"), 1):
        for m in TOKEN.finditer(line):
            out.append((m.group(0), lineno))
    return out


def walk(root, skip=()):
    for d, dirs, files in os.walk(root):
        dirs[:] = [x for x in dirs if x not in skip and not x.startswith(".")]
        for f in files:
            if f.endswith(EXT):
                yield os.path.join(d, f)


def main():
    target_root = sys.argv[1]
    corpora = sys.argv[2:]
    index = defaultdict(list)   # shingle -> [(file, line)]
    for corpus in corpora:
        for path in walk(corpus, skip=("contrib", "imgui", "build", "thirdparty", "glad")):
            t = tokens(path)
            for i in range(len(t) - N + 1):
                key = hash(tuple(tok for tok, _ in t[i:i + N]))
                index[key].append((path, t[i][1]))
    print(f"indexed {len(index)} shingles from {len(corpora)} corpora")
    total = 0
    for path in sorted(walk(target_root)):
        t = tokens(path)
        hits = [None] * len(t)
        for i in range(len(t) - N + 1):
            key = hash(tuple(tok for tok, _ in t[i:i + N]))
            if key in index:
                hits[i] = index[key][0]
        # merge consecutive hit starts into runs
        i = 0
        while i < len(t):
            if hits[i] is None:
                i += 1
                continue
            j = i
            while j + 1 < len(t) and hits[j + 1] is not None:
                j += 1
            run = j - i + N
            if run >= MIN_RUN:
                src_file, src_line = hits[i]
                snippet = " ".join(tok for tok, _ in t[i:i + min(run, 30)])
                print(f"{os.path.relpath(path, target_root)}:{t[i][1]}-{t[min(j + N - 1, len(t) - 1)][1]}  "
                      f"{run} tokens  <=  {src_file}:{src_line}\n    {snippet[:160]}")
                total += 1
            i = j + 1
    print(f"{total} shared runs of >= {MIN_RUN} tokens")


main()

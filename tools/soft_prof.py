"""Summarise a bench-soft profile.

    python tools/soft_prof.py <executable> <file.samples> [top]

Each sample is an address relative to the executable's base. addr2line (with
the image base added) resolves it, inline frames included, and the counts are
printed per source line and per function. Build with -DWETTER_DEBUGINFO=ON
for line tables in Wetter.
"""

import collections
import os
import subprocess
import sys

IMAGE_BASE = 0x140000000


def main():
    exe, samples = sys.argv[1], sys.argv[2]
    top = int(sys.argv[3]) if len(sys.argv) > 3 else 40
    counts = collections.Counter(int(l, 16) for l in open(samples) if l.strip())
    total = sum(counts.values())
    addrs = sorted(counts)
    tool = os.environ.get("ADDR2LINE", "addr2line")
    out = subprocess.run([tool, "-e", exe, "-f", "-C", "-i", "-a"] + ["0x%x" % (a + IMAGE_BASE) for a in addrs],
                         capture_output=True, text=True).stdout.splitlines()

    # -a prints the address, then (function, file:line) pairs, innermost first.
    frames = {}
    cur = None
    i = 0
    while i < len(out):
        line = out[i]
        if line.startswith("0x"):
            cur = int(line, 16) - IMAGE_BASE
            frames[cur] = []
            i += 1
            continue
        fn = line
        loc = out[i + 1] if i + 1 < len(out) else "?"
        frames[cur].append((fn, os.path.basename(loc.split(" ")[0])))
        i += 2

    by_line = collections.Counter()
    by_fn = collections.Counter()
    for a, n in counts.items():
        f = frames.get(a) or [("??", "??")]
        by_line[f[0][1] + "  " + f[0][0][:60]] += n
        seen = set()
        for fn, _ in f:
            if fn not in seen:
                by_fn[fn[:90]] += n
                seen.add(fn)

    print("%d samples" % total)
    print("\n-- innermost line --")
    for k, n in by_line.most_common(top):
        print("%5.1f%%  %s" % (100.0 * n / total, k))
    print("\n-- inclusive per function --")
    for k, n in by_fn.most_common(top):
        print("%5.1f%%  %s" % (100.0 * n / total, k))


if __name__ == "__main__":
    main()

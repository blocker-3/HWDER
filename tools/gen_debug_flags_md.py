"""Print the README's debug-switch table from the table in runtime/src/debug_flags.cpp.

The C++ table is the source of truth; this script only reads it. README.md carries the output between
`<!-- debug-flags:begin -->` and `<!-- debug-flags:end -->`, and tools/tests/test_debug_flags_md.py fails
when the two differ. Run from anywhere:

    python tools/gen_debug_flags_md.py            # print the table
    python tools/gen_debug_flags_md.py --write    # rewrite the README section in place
"""
import argparse
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "runtime" / "src" / "debug_flags.cpp"
README = ROOT / "README.md"
BEGIN = "<!-- debug-flags:begin -->"
END = "<!-- debug-flags:end -->"

# Parse rule of each Kind (runtime/src/debug_flags.h) and what an unset variable means.
KINDS = {
    "kPresence": ("presence", "off", "set to anything (even empty) turns it on"),
    "kOnUnlessZero": ("on unless 0", "on", "only the exact value `0` turns it off"),
    "kNonzero": ("nonzero", "off", "on when set to a value that does not start with `0`"),
    "kInt": ("integer", "unset", "the number is used when set; the site's own default otherwise"),
    "kFloat": ("number", "unset", "the number is used when set; the site's own default otherwise"),
    "kString": ("string", "unset", "the text is used when set; see the meaning for its format"),
}

_STRING = r'"((?:[^"\\]|\\.)*)"'
_ENTRY = re.compile(r"\{\s*(\w+)\s*,\s*" + _STRING + r"\s*,\s*" + _STRING + r"\s*,\s*(k\w+)\s*\}")


def _unescape(s):
    return s.encode("ascii", "backslashreplace").decode("unicode_escape")


def parse(source=SOURCE):
    """[(id, name, meaning, kind)] in table order; raises when the table is missing or malformed."""
    text = source.read_text(encoding="utf-8")
    start = text.index("kTable[] = {")
    body = text[start:text.index("\n};", start)]
    entries = []
    for line in body.splitlines()[1:]:
        line = line.strip()
        if not line or line.startswith("//"):
            continue
        m = _ENTRY.match(line)
        if not m:
            raise ValueError("unparsed debug_flags.cpp table line: " + line)
        ident, name, meaning, kind = m.groups()
        if kind not in KINDS:
            raise ValueError("unknown Kind %s for %s" % (kind, name))
        entries.append((ident, name, _unescape(meaning), kind))
    if not entries:
        raise ValueError("empty debug_flags.cpp table")
    return entries


def _cell(s):
    return s.replace("|", "\\|")


def render(entries):
    lines = [
        "Every `HWDER_*` switch the runtime reads, generated from `runtime/src/debug_flags.cpp` by",
        "`tools/gen_debug_flags_md.py` (do not edit by hand; `python tools/gen_debug_flags_md.py --write`",
        "refreshes it). Each is read once, at startup; the F1 overlay's Tools tab lists them with their",
        "current values. Kinds:",
        "",
    ] + ["- **%s**: %s." % (KINDS[k][0], KINDS[k][2]) for k in KINDS] + [
        "",
        "| Variable | Kind | Default | Meaning |",
        "|---|---|---|---|",
    ]
    for _ident, name, meaning, kind in entries:
        label, default, _ = KINDS[kind]
        lines.append("| `%s` | %s | %s | %s |" % (name, label, default, _cell(meaning)))
    return "\n".join(lines) + "\n"


def generate():
    return render(parse())


def readme_section(text):
    """The text between the markers (exclusive), or None when a marker is missing."""
    b, e = text.find(BEGIN), text.find(END)
    if b < 0 or e < 0 or e < b:
        return None
    return text[b + len(BEGIN):e].strip("\n") + "\n"


def write(readme=README):
    text = readme.read_text(encoding="utf-8")
    b, e = text.find(BEGIN), text.find(END)
    if b < 0 or e < 0 or e < b:
        raise ValueError("README.md lacks the debug-flags markers")
    new = text[:b + len(BEGIN)] + "\n" + generate() + text[e:]
    if new != text:
        readme.write_text(new, encoding="utf-8", newline="\n")
    return new != text


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--write", action="store_true", help="rewrite the README section in place")
    a = ap.parse_args(argv)
    if a.write:
        print("README.md updated" if write() else "README.md already current")
    else:
        sys.stdout.write(generate())
    return 0


if __name__ == "__main__":
    sys.exit(main())

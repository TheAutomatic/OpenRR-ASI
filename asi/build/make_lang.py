#!/usr/bin/env python3
"""The menu's English language file, from the code (asi/src/rr_lang.hpp reads it):
    make_lang.py <out English.txt> <source files...>

Every text the menu translates: the string literals inside tr(...) and tr_items(...). Adjacent literals are joined as
the compiler joins them, and tr_items(...) strings are split on NUL separators. Each text once, in the order found;
msgstr is the English text itself, so a copy of the file is a complete language to translate in place.
"""
import sys
from pathlib import Path

ESC = {"n": "\n", "t": "\t", "0": "\0", "\\": "\\", '"': '"', "'": "'", "r": "\r"}


def tokens(src):
    """(kind, text, pos): kind 's' a string literal (its value), 'i' an identifier, 'p' punctuation; comments and
    preprocessor lines skipped."""
    i, n = 0, len(src)
    while i < n:
        c = src[i]
        if c in " \t\r\n":
            i += 1
        elif src.startswith("//", i):
            i = src.find("\n", i)
            i = n if i < 0 else i
        elif src.startswith("/*", i):
            i = src.find("*/", i) + 2
        elif c == "#" and (i == 0 or src[src.rfind("\n", 0, i) + 1:i].strip() == ""):
            while True:   # a preprocessor line, with its continuations
                j = src.find("\n", i)
                if j < 0:
                    i = n
                    break
                if src[j - 1] != "\\":
                    i = j
                    break
                i = j + 1
        elif c == "'":
            i += 1
            while i < n and src[i] != "'":
                i += 2 if (src[i] == "\\" and i + 1 < n) else 1
            i += 1
            yield "p", "'", i
        elif c == '"' or (c in "uUL" and src[i + 1:i + 2] == '"') or (src.startswith("u8", i) and src[i + 2:i + 3] == '"'):
            if src.startswith('R"', i):
                raise SystemExit("raw string literals are not read")
            i = src.index('"', i) + 1
            v = ""
            while i < n and src[i] != '"':
                if src[i] == "\\" and i + 1 < n and src[i + 1] in ESC:
                    v += ESC[src[i + 1]]
                    i += 2
                else:
                    v += src[i]
                    i += 1
            i += 1
            yield "s", v, i
        elif c.isalpha() or c == "_":
            j = i
            while j < n and (src[j].isalnum() or src[j] == "_"):
                j += 1
            yield "i", src[i:j], i
            i = j
        else:
            yield "p", c, i
            i += 1


def texts_of(src):
    """The literals inside tr(...) and tr_items(...): adjacent literals joined as the compiler joins them."""
    out = []
    toks = list(tokens(src))
    i = 0
    while i < len(toks):
        kind, v, _ = toks[i]
        if kind != "i" or v not in ("tr", "tr_items"):
            i += 1
            continue
        j = i + 1
        while j < len(toks) and toks[j][0] == "p" and toks[j][1] in ".>":   # a member namespace
            j += 2
            if j - 1 >= len(toks):
                j = len(toks)
                break
        if j >= len(toks) or not (toks[j][0] == "p" and toks[j][1] == "("):
            i += 1
            continue
        depth, k, lit, started = 0, j, "", False
        while k < len(toks):
            kind, v2, _ = toks[k]
            if kind == "p":
                if v2 == "(":
                    depth += 1
                elif v2 == ")":
                    depth -= 1
                    if depth == 0:
                        break
                elif v2 == "," and depth == 1:
                    lit, started = "", False   # only the first argument
            elif kind == "s" and depth == 1 and not started:
                lit += v2
                started = True
            elif kind == "s" and depth == 1:
                lit += v2   # adjacent literals join
            k += 1
        if started and lit:
            if v == "tr_items":
                for part in lit.split("\0"):
                    if part:
                        out.append(part)
            else:
                out.append(lit)
        i = k + 1
    return out


def main():
    if len(sys.argv) < 3:
        print(__doc__, file=sys.stderr)
        return 2
    out, texts = Path(sys.argv[1]), []
    for arg in sys.argv[2:]:
        texts += texts_of(Path(arg).read_text(encoding="utf-8"))
    seen = set()
    with out.open("w", encoding="utf-8", newline="\n") as f:
        f.write('# OpenRR: the menu\'s texts in English, made from the code by asi/build/make_lang.py.\n'
                 '# A copy with its msgstr lines translated is a new language (openrr-amd\\lang\\<name>.txt,\n'
                 '# [OpenRR] Language in openrr-amd.ini); msgid "@language" names it in the menu, msgid "@font" its\n'
                 '# letters (openrr-amd\\lang\\fonts\\..., else the Windows CJK fonts, else English stays).\n\n'
                 'msgid "@language"\nmsgstr "English"\n\n')
        for t in texts:
            if t in seen:
                continue
            seen.add(t)
            esc = t.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n").replace("\t", "\\t")
            f.write(f'msgid "{esc}"\nmsgstr "{esc}"\n\n')
    print(f"{out}: {len(seen)} texts")
    return 0


if __name__ == "__main__":
    sys.exit(main())

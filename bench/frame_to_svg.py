#!/usr/bin/env python3
"""Turn a frame from `arena --still` into an SVG, for the README.

    build/release/arena --still=60000000 --seed=42 > frame.ansi
    python bench/frame_to_svg.py frame.ansi docs/arena.svg

Why an SVG and not a screenshot: a PNG is a thing somebody took once, and nobody
can tell whether it still matches the code. This is generated from a seed and a
simulated instant, both printed into the image, so anyone can regenerate it and
diff it. It needs no recording tool, no asciinema, and no dependency outside the
standard library, which is the same rule the rest of the repository follows.

Parses only what arena/ui.hpp emits: cursor-home, 24-bit foreground and
background SGR, and reset. Anything else is dropped rather than guessed at.
"""
import io
import re
import sys

SGR = re.compile(r"\x1b\[([0-9;]*)m")
CELL_W = 8.4
CELL_H = 17.0
PAD = 14.0


def escape(s):
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;").replace('"', "&quot;")


def parse(path):
    """-> (rows, width) where each row is a list of (fg, bg, text) runs."""
    raw = io.open(path, encoding="utf-8", newline="").read()
    raw = raw.replace("\x1b[H", "")
    raw = raw.split("\nstill at ")[0]
    rows, width = [], 0
    for line in raw.split("\n"):
        line = line.rstrip("\r")
        runs, fg, bg, pos, col = [], None, None, 0, 0
        for m in SGR.finditer(line):
            text = line[pos:m.start()]
            if text:
                runs.append((fg, bg, text))
                col += len(text)
            codes = [c for c in m.group(1).split(";") if c]
            i = 0
            while i < len(codes):
                c = codes[i]
                if c == "0":
                    fg = bg = None
                    i += 1
                elif c in ("38", "48") and i + 4 < len(codes) and codes[i + 1] == "2":
                    rgb = "#%02x%02x%02x" % (int(codes[i + 2]), int(codes[i + 3]), int(codes[i + 4]))
                    if c == "38":
                        fg = rgb
                    else:
                        bg = rgb
                    i += 5
                else:
                    i += 1
            pos = m.end()
        tail = line[pos:]
        if tail:
            runs.append((fg, bg, tail))
            col += len(tail)
        rows.append(runs)
        width = max(width, col)
    while rows and not any(t.strip() for _, _, t in rows[0]):
        rows.pop(0)
    while rows and not any(t.strip() for _, _, t in rows[-1]):
        rows.pop()
    return rows, width


def render(rows, width, caption):
    w = PAD * 2 + width * CELL_W
    h = PAD * 2 + len(rows) * CELL_H + (22 if caption else 0)
    out = [
        '<svg xmlns="http://www.w3.org/2000/svg" width="%.0f" height="%.0f" '
        'viewBox="0 0 %.0f %.0f" role="img" aria-label="%s">' % (w, h, w, h, escape(caption or "arena")),
        "<style>",
        # One monospace stack, and a fixed advance width so the box-drawing joins
        # up wherever it is viewed. No webfont: an SVG in a README cannot fetch one.
        "  text{font-family:'Cascadia Mono',Consolas,'DejaVu Sans Mono','Liberation Mono',"
        "ui-monospace,monospace;font-size:13px;white-space:pre}",
        "  .c{fill:#9a89c4;font-size:11px}",
        "</style>",
        '<rect width="100%" height="100%" fill="#2a1b4a"/>',
    ]
    # Backgrounds first, as one rect per run, then the glyphs over them.
    for r, runs in enumerate(rows):
        y = PAD + r * CELL_H
        col = 0
        for fg, bg, text in runs:
            if bg:
                out.append('<rect x="%.2f" y="%.2f" width="%.2f" height="%.2f" fill="%s"/>'
                           % (PAD + col * CELL_W, y, len(text) * CELL_W + 0.4, CELL_H + 0.4, bg))
            col += len(text)
    for r, runs in enumerate(rows):
        y = PAD + r * CELL_H + CELL_H - 4.5
        col = 0
        for fg, bg, text in runs:
            if text.strip():
                out.append('<text x="%.2f" y="%.2f" fill="%s" textLength="%.2f" lengthAdjust="spacingAndGlyphs">%s</text>'
                           % (PAD + col * CELL_W, y, fg or "#e8dfff", len(text) * CELL_W, escape(text)))
            col += len(text)
    if caption:
        out.append('<text class="c" x="%.2f" y="%.2f">%s</text>'
                   % (PAD, PAD + len(rows) * CELL_H + 14, escape(caption)))
    out.append("</svg>")
    return "\n".join(out)


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2
    rows, width = parse(argv[1])
    if not rows:
        print("no frame in %s" % argv[1], file=sys.stderr)
        return 1
    caption = argv[3] if len(argv) > 3 else ""
    svg = render(rows, width, caption)
    io.open(argv[2], "w", encoding="utf-8", newline="\n").write(svg + "\n")
    print("%s -> %s (%d rows x %d cols, %.1f KB)"
          % (argv[1], argv[2], len(rows), width, len(svg.encode("utf-8")) / 1024.0))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))

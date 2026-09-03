#!/usr/bin/env python3
# ============================================================================
# render_trace.py
#
# Wraps build/book_trace.jsonl (one JSON object per tick, from
# tools/book_trace.c) into tools/visualizer_template.html, producing a
# single self-contained HTML file with the trace data embedded directly —
# no server, no fetch, just `open` it in a browser. Kept as a separate
# template/script pair rather than having book_trace.c print HTML directly,
# so the page's CSS/JS can be edited as plain HTML instead of escaped C
# string literals.
# ============================================================================
import json
import sys

def main():
    trace_path = sys.argv[1] if len(sys.argv) > 1 else "build/book_trace.jsonl"
    template_path = sys.argv[2] if len(sys.argv) > 2 else "tools/visualizer_template.html"
    out_path = sys.argv[3] if len(sys.argv) > 3 else "build/book_visualizer.html"

    ticks = []
    with open(trace_path) as f:
        for line in f:
            line = line.strip()
            if line:
                ticks.append(json.loads(line))

    with open(template_path) as f:
        template = f.read()

    placeholder = "__TRACE_JSON__"
    if placeholder not in template:
        sys.exit("render_trace.py: placeholder %r not found in template" % placeholder)

    html = template.replace(placeholder, json.dumps(ticks))

    with open(out_path, "w") as f:
        f.write(html)

    print("wrote %s (%d ticks)" % (out_path, len(ticks)))

if __name__ == "__main__":
    main()

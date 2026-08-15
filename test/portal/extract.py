#!/usr/bin/env python3
"""Pull the HTML blob out of cypher32_portal.h so it can be linted and tested."""
import re, sys, pathlib
src = pathlib.Path(__file__).parent.parent.parent / "cypher32_portal.h"
s = src.read_text(encoding="utf-8")
m = re.search(r'R"PORTAL\((.*)\)PORTAL"', s, re.S)
if not m:
    sys.exit("could not find the PORTAL raw string literal")
html = m.group(1)
if ')PORTAL"' in html:
    sys.exit("raw-string delimiter appears inside the literal — it would not compile")
out = pathlib.Path(__file__).parent
(out / "portal.html").write_text(html, encoding="utf-8")
js = re.search(r"<script>(.*?)</script>", html, re.S).group(1)
(out / "portal.js").write_text(js, encoding="utf-8")

# every id the script reaches for must exist in the markup
dom  = set(re.findall(r'\bid="([^"]+)"', html))
used = set(re.findall(r'\$\("([^"]+)"\)', js))
missing = used - dom
if missing:
    sys.exit("script references element ids that do not exist: " + ", ".join(sorted(missing)))
print(f"portal: {len(html)} bytes html, {len(js)} bytes js, "
      f"{len(used)} element ids all present")

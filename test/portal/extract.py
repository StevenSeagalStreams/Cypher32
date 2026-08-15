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
# A utility class like .hide has the same specificity as any other single
# class, so if something it is combined with declares `display` later in the
# stylesheet, that wins and the element never hides. This shipped once: .modal
# declared display:flex after .hide, leaving the password overlay permanently
# on top of the setup wizard. Catch it structurally rather than by eye.
css = re.search(r"<style>(.*?)</style>", html, re.S).group(1)
rules = [(m.start(), m.group(1), m.group(2))
         for m in re.finditer(r"\.([A-Za-z0-9_-]+)\s*\{([^}]*)\}", css)]

def declares_display(body):
    return re.search(r"(^|;)\s*display\s*:", body) is not None

hide_rules = [(pos, body) for pos, name, body in rules if name == "hide"]
if not hide_rules:
    sys.exit("no .hide rule found")
hide_pos, hide_body = hide_rules[-1]
hide_wins_always = "!important" in hide_body

problems = []
if not hide_wins_always:
    for cls_attr in re.findall(r'class="([^"]*\bhide\b[^"]*)"', html):
        for other in cls_attr.split():
            if other == "hide":
                continue
            for pos, name, body in rules:
                if name == other and declares_display(body) and pos > hide_pos:
                    problems.append(f".{other} declares display after .hide — "
                                    f'class="{cls_attr}" will not hide')
if problems:
    sys.exit("CSS specificity problem:\n  " + "\n  ".join(sorted(set(problems))))

print(f"portal: {len(html)} bytes html, {len(js)} bytes js, "
      f"{len(used)} element ids all present, .hide safe")

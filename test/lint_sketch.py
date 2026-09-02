#!/usr/bin/env python3
"""Catch undeclared ALL-CAPS constants in the sketch.

There is no ESP32 toolchain in this repo's test environment, so the .ino is
never compiled here. That let a real bug ship: replacing a block of code by
index also deleted `#define MAX_LEVEL 32`, which sat between two functions, and
nothing noticed until the Arduino IDE refused to build.

This is not a compiler. It only checks that every ALL-CAPS identifier the
sketch uses is defined somewhere in the project or is a known platform symbol —
which is exactly the class of mistake that slipped through.
"""
import re, sys, pathlib

root = pathlib.Path(__file__).parent.parent
sources = sorted(list(root.glob("*.ino")) + list(root.glob("*.h")))
if not sources:
    sys.exit("no sketch sources found")

# Symbols the Arduino core, ESP32 core and our libraries provide.
PLATFORM = {
    "HIGH", "LOW", "INPUT", "OUTPUT", "INPUT_PULLUP", "PROGMEM", "IRAM_ATTR",
    "HTTP_GET", "HTTP_POST", "HTTP_ANY", "WIFI_AP", "WIFI_STA", "WIFI_AP_STA",
    "FSPI", "HSPI", "VSPI", "SPI", "LED_BUILTIN", "A0", "DEC", "HEX", "BIN",
    "BLACK", "WHITE", "SS", "MOSI", "MISO", "SCK",
    "NULL", "UINT32_MAX", "INT32_MAX", "UINT8_MAX", "INT8_MAX", "SIZE_MAX",
    "ESP", "Serial", "WiFi", "MDNS",   # Arduino global objects
    "CHANGE", "RISING", "FALLING",     # attachInterrupt modes
}
PLATFORM_PREFIXES = ("RADIOLIB_", "ESP_", "ARDUINO_", "SX126", "DNS", "MDNS", "WL_")

defined = set(PLATFORM)
for f in sources:
    text = f.read_text(encoding="utf-8", errors="replace")
    defined |= set(re.findall(r"^\s*#define\s+([A-Za-z_]\w*)", text, re.M))
    # Any const declaration, however it is spelled. The two narrow patterns
    # this replaces missed a pointer type ("const char* LBL[]") and every
    # declarator after the first ("const int TRACK_X = 58, TRACK_W = 170"),
    # which made the linter reject perfectly ordinary C++ and pushed you
    # towards renaming real constants to keep it quiet.
    for decl in re.findall(r"\bconst\b[^;{}]*", text):
        defined |= set(re.findall(r"\b([A-Z][A-Z0-9_]{2,})\s*(?:=|\[)", decl))
    # enum bodies, e.g. enum Foo { A_B, C_D };
    for body in re.findall(r"\benum\s+\w*\s*\{([^}]*)\}", text, re.S):
        defined |= set(re.findall(r"\b([A-Z][A-Z0-9_]{2,})\b", body))

# Strip the HTML blob, then literals, then comments — in that order.
#
# Order matters. Removing line comments first breaks on a URL like
# "http://192.168.4.1/": the // is eaten as a comment, the closing quote goes
# with it, and every string literal after that point is misparsed. That is what
# this linter did on its first run, and it reported twenty phantom errors.
#
# Substitutions keep the original newline count so reported line numbers still
# point at the real source line.
def _blank(m):
    return "\n" * m.group(0).count("\n")

def strip(text):
    text = re.sub(r"R\"PORTAL\(.*?\)PORTAL\"", _blank, text, flags=re.S)
    text = re.sub(r'"(?:\\.|[^"\\\n])*"', _blank, text)      # strings first
    text = re.sub(r"'(?:\\.|[^'\\\n])'", " ", text)          # then char literals
    text = re.sub(r"/\*.*?\*/", _blank, text, flags=re.S)     # then comments
    text = re.sub(r"//[^\n]*", " ", text)
    return text

problems = []
for f in sources:
    body = strip(f.read_text(encoding="utf-8", errors="replace"))
    # Drop the #define lines themselves so a definition is not read as a use.
    body = re.sub(r"^\s*#\s*(define|include|ifn?def|if|elif|endif|else|pragma).*$",
                  " ", body, flags=re.M)
    for m in re.finditer(r"\b([A-Z][A-Z0-9_]{2,})\b", body):
        name = m.group(1)
        if name in defined:                              continue
        if name.startswith(PLATFORM_PREFIXES):           continue
        line = body[:m.start()].count("\n") + 1
        problems.append(f"{f.name}:{line}: '{name}' is used but never defined")

if problems:
    print("Undeclared constants — the sketch will not compile:", file=sys.stderr)
    for p in sorted(set(problems)):
        print("  " + p, file=sys.stderr)
    sys.exit(1)

print(f"sketch lint: {len(sources)} files, all ALL-CAPS constants resolve")

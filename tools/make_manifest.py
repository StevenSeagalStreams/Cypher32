#!/usr/bin/env python3
"""Assemble the browser flasher: manifests, binaries and a build record.

Run from the repository root:

    tools/make_manifest.py --artifacts artifacts --out site \\
        --version v71 --commit "$GITHUB_SHA"

Kept out of the workflow file so it can be run and tested without pushing a
commit and waiting for CI to tell you about a typo.
"""
import argparse
import datetime
import json
import os
import shutil
import sys

PROFILES = ("fast", "long", "epic")

# esptool's chip name -> the chipFamily string ESP Web Tools matches against
# the connected device. A mismatch here is not a soft failure: the flasher
# refuses the device and says the manifest has no build for it.
FAMILY = {
    "esp32":   "ESP32",
    "esp32s2": "ESP32-S2",
    "esp32s3": "ESP32-S3",
    "esp32c3": "ESP32-C3",
    "esp32c6": "ESP32-C6",
}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--artifacts", required=True,
                    help="directory holding cypher32-<profile>/ subdirectories")
    ap.add_argument("--out", required=True)
    ap.add_argument("--version", required=True)
    ap.add_argument("--commit", default="")
    ap.add_argument("--page", default="web/index.html")
    ap.add_argument("--summary", default="",
                    help="also write a markdown table here (GITHUB_STEP_SUMMARY)")
    args = ap.parse_args()

    os.makedirs(args.out, exist_ok=True)
    shutil.copy(args.page, os.path.join(args.out, "index.html"))

    builds = []
    for profile in PROFILES:
        src = os.path.join(args.artifacts, "cypher32-%s" % profile)
        binary = os.path.join(src, "cypher32-%s.bin" % profile)
        meta_path = os.path.join(src, "parts-%s.json" % profile)
        for required in (binary, meta_path):
            if not os.path.isfile(required):
                print("missing %s" % required, file=sys.stderr)
                return 1

        with open(meta_path) as f:
            meta = json.load(f)
        chip = meta.get("chip", "")
        if chip not in FAMILY:
            print("unrecognised chip %r in %s" % (chip, meta_path), file=sys.stderr)
            return 1

        shutil.copy(binary, os.path.join(args.out, "cypher32-%s.bin" % profile))

        # The image is merged, so every part offset is already baked into it
        # and the whole thing is written at zero.
        manifest = {
            "name": "Cypher32 (%s)" % profile.upper(),
            "version": args.version,
            "new_install_prompt_erase": True,
            "builds": [{
                "chipFamily": FAMILY[chip],
                "parts": [{"path": "cypher32-%s.bin" % profile, "offset": 0}],
            }],
        }
        with open(os.path.join(args.out, "manifest-%s.json" % profile), "w") as f:
            json.dump(manifest, f, indent=2)

        size = os.path.getsize(binary)
        builds.append({"profile": profile, "bytes": size,
                       "app_bytes": meta.get("app_bytes", 0),
                       "chipFamily": FAMILY[chip]})
        print("  %-5s %7d B  %s" % (profile, size, FAMILY[chip]))

    with open(os.path.join(args.out, "build-info.json"), "w") as f:
        json.dump({
            "version": args.version,
            "commit": args.commit,
            "built": datetime.datetime.now(datetime.timezone.utc)
                             .strftime("%Y-%m-%d"),
            "builds": builds,
        }, f, indent=2)

    # A flasher that quietly serves a stale or missing binary is worse than one
    # that is plainly broken, so refuse to publish a half-assembled site.
    expected = ["index.html", "build-info.json"]
    for profile in PROFILES:
        expected += ["cypher32-%s.bin" % profile, "manifest-%s.json" % profile]
    for name in expected:
        path = os.path.join(args.out, name)
        if not os.path.isfile(path) or os.path.getsize(path) == 0:
            print("site/%s missing or empty" % name, file=sys.stderr)
            return 1

    # The app partition from default_8MB.csv. Worth stating as a percentage:
    # a build creeping towards the partition size is the kind of thing nobody
    # notices until an image silently stops fitting.
    APP_PARTITION = 0x330000

    table = ["| profile | image | application | of %.2f MB app partition |"
             % (APP_PARTITION / 1048576.0),
             "|---|---|---|---|"]
    for b in builds:
        table.append("| %s | %.0f kB | %.0f kB | %.1f%% |" % (
            b["profile"].upper(), b["bytes"] / 1024.0,
            b["app_bytes"] / 1024.0, b["app_bytes"] * 100.0 / APP_PARTITION))
    table.append("")
    table.append("firmware %s from %s" % (args.version, (args.commit or "?")[:7]))

    over = [b for b in builds if b["app_bytes"] > APP_PARTITION]
    if over:
        print("application does not fit the app partition: %s"
              % ", ".join(b["profile"] for b in over), file=sys.stderr)
        return 1

    print("\n".join(table))
    if args.summary:
        with open(args.summary, "a") as f:
            f.write("### Flasher published\n\n" + "\n".join(table) + "\n")

    print("flasher assembled in %s/" % args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())

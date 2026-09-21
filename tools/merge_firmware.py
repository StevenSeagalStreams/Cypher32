# PlatformIO post-build step: produce one flashable image per range profile.
#
# A browser flasher needs a single binary it can write at offset 0. The parts
# PlatformIO produces — bootloader, partition table, boot_app0, application —
# each live at a different offset, and those offsets are NOT the same across
# the ESP32 family: the second-stage bootloader sits at 0x1000 on the original
# ESP32 and at 0x0 on the S3 and C3. Getting one wrong produces a binary that
# flashes without complaint and then boots into nothing.
#
# So nothing here is hardcoded. PlatformIO already knows every offset it is
# going to use — FLASH_EXTRA_IMAGES holds the (offset, image) pairs for the
# bootloader, the partition table and boot_app0, and ESP32_APP_OFFSET is where
# the application goes. We ask, rather than guess, and print what we were told
# so a CI log shows the real layout.
Import("env")  # noqa: F821  (injected by PlatformIO)

import json
import os
import subprocess
import sys


def _merge(source, target, env):
    build_dir = env.subst("$BUILD_DIR")
    env_name = env.subst("$PIOENV")
    app = os.path.join(build_dir, "firmware.bin")

    parts = []
    for offset, image in env.get("FLASH_EXTRA_IMAGES", []):
        parts.append((int(str(offset), 0), env.subst(image)))
    parts.append((int(env.subst("$ESP32_APP_OFFSET"), 0), app))
    parts.sort(key=lambda p: p[0])

    missing = [p for _, p in parts if not os.path.isfile(p)]
    if missing:
        sys.stderr.write("merge_firmware: missing %s\n" % ", ".join(missing))
        env.Exit(1)

    # Overlap would silently corrupt the image, and the failure mode is a
    # device that flashes cleanly and never boots.
    for (a_off, a_path), (b_off, _) in zip(parts, parts[1:]):
        a_end = a_off + os.path.getsize(a_path)
        if a_end > b_off:
            sys.stderr.write(
                "merge_firmware: %s ends at 0x%x, past the next part at 0x%x\n"
                % (a_path, a_end, b_off))
            env.Exit(1)

    out = os.path.join(build_dir, "cypher32-%s.bin" % env_name)
    board = env.BoardConfig()
    cmd = [
        env.subst("$PYTHONEXE"), env.subst("$OBJCOPY"),
        "--chip", board.get("build.mcu", "esp32s3"),
        "merge_bin", "-o", out,
        "--flash_mode", board.get("build.flash_mode", "dio"),
        "--flash_freq", board.get("build.f_flash", "80000000L").replace("000000L", "m"),
        "--flash_size", board.get("upload.flash_size", "8MB"),
    ]
    for offset, path in parts:
        cmd += [hex(offset), path]

    print("merge_firmware: layout for %s" % env_name)
    for offset, path in parts:
        print("    0x%06x  %8d B  %s" % (offset, os.path.getsize(path),
                                         os.path.basename(path)))
    subprocess.check_call(cmd)
    print("merge_firmware: wrote %s (%d bytes)" % (out, os.path.getsize(out)))

    # Hand the real layout to the manifest generator rather than making it
    # repeat the same guesses this script exists to avoid.
    meta = {
        "env": env_name,
        "chip": board.get("build.mcu", "esp32s3"),
        "merged": os.path.basename(out),
        "merged_bytes": os.path.getsize(out),
        "app_bytes": os.path.getsize(app),
        "parts": [{"offset": o, "file": os.path.basename(p),
                   "bytes": os.path.getsize(p)} for o, p in parts],
    }
    with open(os.path.join(build_dir, "parts-%s.json" % env_name), "w") as f:
        json.dump(meta, f, indent=2)


env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", _merge)  # noqa: F821

# PlatformIO post-build script: copies the compiled binaries into release/.
#   release/module-tester-firmware-<ver>.bin  app only (flash at 0x10000)
#   release/module-tester-merged-<ver>.bin    bootloader + partitions + app, flash at 0x0
Import("env")  # noqa: F821  (provided by PlatformIO)

import os
import re
import shutil
import subprocess
import sys


def version(project_dir):
    try:
        with open(os.path.join(project_dir, "src", "core", "ui.h")) as f:
            m = re.search(r'kVersion\s*=\s*"([^"]+)"', f.read())
            if m:
                return m.group(1)
    except OSError:
        pass
    return "dev"


def copy_release(source, target, env):
    project = env.subst("$PROJECT_DIR")
    build = env.subst("$BUILD_DIR")
    out = os.path.join(project, "release")
    os.makedirs(out, exist_ok=True)
    ver = version(project)
    base = "module-tester-cardputer-adv"

    app = os.path.join(build, "firmware.bin")
    if not os.path.exists(app):
        print("release: %s not found, nothing copied" % app)
        return
    app_out = os.path.join(out, base + "-firmware-%s.bin" % ver)
    shutil.copyfile(app, app_out)
    print("release: wrote %s" % app_out)

    # Merged image so the whole thing can be flashed at 0x0 in one go.
    try:
        platform = env.PioPlatform()
        esptool = os.path.join(platform.get_package_dir("tool-esptoolpy"), "esptool.py")
        framework = platform.get_package_dir("framework-arduinoespressif32")
        boot_app0 = os.path.join(framework, "tools", "partitions", "boot_app0.bin")
        bootloader = os.path.join(build, "bootloader.bin")
        partitions = os.path.join(build, "partitions.bin")
        merged = os.path.join(out, base + "-launcher-%s.bin" % ver)
        # esptool wants e.g. "8MB"; use the board's own size, or keep what the
        # bootloader already says.
        flash_size = str(env.BoardConfig().get("upload.flash_size", "keep"))
        if flash_size not in ("keep", "256KB", "512KB", "1MB", "2MB", "4MB", "8MB",
                              "16MB", "32MB", "64MB", "128MB"):
            flash_size = "keep"
        cmd = [
            env.subst("$PYTHONEXE"), esptool,
            "--chip", env.BoardConfig().get("build.mcu", "esp32s3"),
            "merge_bin", "-o", merged,
            "--flash_mode", env.BoardConfig().get("build.flash_mode", "dio"),
            "--flash_size", flash_size,
            "0x0", bootloader,
            "0x8000", partitions,
            "0xe000", boot_app0,
            "0x10000", app,
        ]
        subprocess.check_call(cmd)
        print("release: wrote %s" % merged)
    except Exception as e:  # never fail the build because of this
        print("release: merged image skipped (%s)" % e, file=sys.stderr)


env.AddPostAction("$BUILD_DIR/firmware.bin", copy_release)  # noqa: F821
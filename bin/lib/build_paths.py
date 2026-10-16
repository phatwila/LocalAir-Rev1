#!/usr/bin/env python3
"""Prepare paths accepted by the pinned Device OS makefiles.

Only the returned temporary directory belongs to this helper. The caller
removes it on exit. Original source and checkout paths remain the provenance
owners; application outputs pass through the alias into the original target.
"""

import os
from pathlib import Path
import re
import shutil
import sys
import tempfile


def make_safe(path):
    return re.fullmatch(r"[/A-Za-z0-9_.-]+", str(path)) is not None


def prepare(application, device_os):
    application = Path(application).resolve(strict=True)
    device_os = Path(device_os).resolve(strict=True)
    # TMPDIR can itself contain spaces. Use an existing safe system temporary
    # directory in that case, never a fixed user or checkout location.
    candidates = [Path(tempfile.gettempdir()), Path("/tmp"), Path("/var/tmp")]
    parent = next((p.resolve() for p in candidates if p.is_dir()
                   and make_safe(p.resolve()) and os.access(p, os.W_OK)), None)
    if parent is None:
        raise ValueError("No writable temporary directory compatible with Device OS")
    stage = Path(tempfile.mkdtemp(prefix="localair-build-paths-", dir=parent))
    try:
        build_app = application
        if not make_safe(application):
            build_app = stage / "app"
            build_app.symlink_to(application, target_is_directory=True)
        build_os = device_os
        if not make_safe(device_os):
            # Make canonicalizes Device OS include paths. A symlink does not
            # protect a spaced physical checkout, so preserve a physical copy.
            build_os = stage / "device-os"
            shutil.copytree(device_os, build_os, symlinks=True)
        (stage / "app-path").write_text(str(build_app) + "\n")
        (stage / "device-os-path").write_text(str(build_os) + "\n")
        return stage
    except BaseException:
        shutil.rmtree(stage)
        raise


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit("Usage: build_paths.py <application> <device-os>")
    print(prepare(*sys.argv[1:]))

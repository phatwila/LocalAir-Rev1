#!/usr/bin/env python3

"""Pack dashboard.html into a reproducible, bounded C gzip blob."""

import gzip
import os
import tempfile


MAX_GZIP_BYTES = 20000
repo_root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
input_file = os.path.join(repo_root, "src", "web", "dashboard.html")
output_file = os.path.join(repo_root, "src", "web", "dashboard.h")

with open(input_file, "rb") as source:
    html_data = source.read()

# Fixed mtime and level improve reproducibility. Exact bytes require pinned
# Python and zlib versions. Keep gzip's native OS byte unchanged.
gz_data = gzip.compress(html_data, compresslevel=9, mtime=0)
length = len(gz_data)
if length > MAX_GZIP_BYTES:
    raise SystemExit(f"dashboard gzip payload exceeds budget: {length} > {MAX_GZIP_BYTES}")

hex_array = []
for offset in range(0, length, 16):
    chunk = gz_data[offset : offset + 16]
    line = ", ".join(f"0x{byte:02x}" for byte in chunk)
    if offset + 16 < length:
        line += ","
    hex_array.append(f"    {line}")

c_content = f"""#pragma once
#include <stdint.h>

// Embedded firmware dashboard HTML payload.
// This is a generated gzip blob; see README.md for the recommended
// edit/rebuild workflow and HTTP gzip header requirements.
static const uint8_t DASHBOARD_HTML_GZ[] = {{
{chr(10).join(hex_array)}
}};

static const uint16_t DASHBOARD_HTML_GZ_LEN = {length};
""".encode("utf-8")

try:
    with open(output_file, "rb") as existing:
        if existing.read() == c_content:
            print(f"Dashboard header unchanged: {output_file}")
            raise SystemExit(0)
except FileNotFoundError:
    pass

try:
    output_mode = os.stat(output_file).st_mode & 0o777
except FileNotFoundError:
    output_mode = 0o644

fd, temporary_file = tempfile.mkstemp(
    prefix=".dashboard.h.", dir=os.path.dirname(output_file)
)
try:
    os.fchmod(fd, output_mode)
    with os.fdopen(fd, "wb") as generated:
        fd = None
        generated.write(c_content)
        generated.flush()
        os.fsync(generated.fileno())
    os.replace(temporary_file, output_file)
    temporary_file = None
finally:
    if fd is not None:
        os.close(fd)
    if temporary_file is not None:
        os.unlink(temporary_file)

print(
    f"Successfully packed {len(html_data)} bytes of HTML into "
    f"{length} bytes of gzip header at {output_file}"
)

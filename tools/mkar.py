#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# mkar.py OUT MEMBER...: an ar archive whose member names have no trailing "/" (what the TV's BusyBox `ar` and the kit's
# ipks use; GNU `ar rc` writes "name/" and the TV answers "ar: unrecognized file type").
import os, sys
out, members = sys.argv[1], sys.argv[2:]
with open(out, "wb") as f:
    f.write(b"!<arch>\n")
    for m in members:
        data = open(m, "rb").read()
        name = os.path.basename(m)
        f.write(b"%-16s%-12d%-6d%-6d%-8s%-10d`\n" % (name.encode(), 1262304000, 0, 0, b"100644", len(data)))
        f.write(data)
        if len(data) % 2: f.write(b"\n")

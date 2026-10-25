#!/usr/bin/env python3
"""Build a .datax containing only the test cases for the functions this patch
touches, so the suite reaches them instead of aborting on an unrelated
pre-existing MemSan report in exp_mod_precompute_window.

MSan cannot be told to continue after an error unless the code was built with
-fsanitize-recover=memory, so filtering the input is the way to get a clean
per-function verdict.
"""
import io
import re
import sys

src, dst = sys.argv[1], sys.argv[2]
want = re.compile(r'^(mbedtls_)?mpi_core_(sub|mla|montmul)\b')

text = io.open(src, encoding="utf-8").read()
blocks = text.split("\n\n")
kept = [b for b in blocks if b.strip() and want.match(b.strip().split("\n")[0])]

io.open(dst, "w", encoding="utf-8", newline="").write("\n\n".join(kept) + "\n\n")
print("kept %d of %d test-case blocks" % (len(kept), len([b for b in blocks if b.strip()])))

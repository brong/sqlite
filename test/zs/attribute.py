#!/usr/bin/env python3
# Attribute a SHARED leaf symbol in macOS `sample` output to the subsystem
# that called it, which a flat top-of-stack table cannot do.
#
# Written by zeroskip's side of this collaboration (2026-08-18) after we each
# misread the same profile in opposite directions: their "fold is under 2%"
# was its SELF time, our "fold is 7%" credited it with decodes belonging to
# three other callers.  The rule it encodes: a flat table answers which
# SYMBOL burned time, never which SUBSYSTEM did, and zsi_rec_decode, memcmp
# and memset each have several callers.
#
#   ./test/zs/attribute.py sample.txt zsi_rec_decode
#
# For a subsystem view, sum sample's own inclusive call-graph counts instead;
# this is only for shared leaves.  OWNERS lists the frames worth attributing
# to -- library internals first, then this engine's own, so the same script
# works on a profile of either side.
import re, sys

OWNERS = [
    # zeroskip internals
    "zsi_index_fold_run", "zsi_index_flush_delta", "zsi_repack_run",
    "zsi_convert_one", "zsi_unordered_replay", "zsi_pend_lb",
    "zsi_txn_terminate", "zsi_txn_at", "zsi_txn_commit", "zs_txn_store",
    # this engine, for profiles taken through SQL
    "zsbtWrite", "zsbtPointFetch", "zsbtLoadValue", "zsbtCursorNext",
    "sqlite3BtreeInsert", "sqlite3BtreeDelete", "sqlite3BtreeTableMoveto",
    "sqlite3BtreeIndexMoveto", "sqlite3VdbeExec",
]

def parse(path):                      # (depth, samples, symbol)
    on = False
    for line in open(path):
        if line.startswith("Call graph:"): on = True; continue
        if not on: continue
        if line.startswith(("Total number in stack", "Sort by top")): break
        m = re.match(r"^(\s*)([+!:|\s]*?)(\d+)\s+(\S+)", line)
        if m: yield len(re.findall(r"[+!:|]", m.group(2))), int(m.group(3)), m.group(4)

if len(sys.argv) != 3:
    sys.exit("usage: attribute.py SAMPLE_OUTPUT LEAF_SYMBOL")
leaf, stack, owned = sys.argv[2], {}, {}
for depth, samples, sym in parse(sys.argv[1]):
    stack[depth] = sym
    for d in [d for d in stack if d > depth]: del stack[d]
    if sym == leaf:
        owner = next((stack[d] for d in sorted(stack, reverse=True)
                      if d < depth and stack[d] in OWNERS), "(other)")
        owned[owner] = owned.get(owner, 0) + samples
total = sum(owned.values())
if not total:
    sys.exit(f"no samples attributed to {leaf} (is it a leaf in this profile?)")
for k, v in sorted(owned.items(), key=lambda kv: -kv[1]):
    print(f"  {v:6d}  {100.0*v/total:5.1f}%  {k}")

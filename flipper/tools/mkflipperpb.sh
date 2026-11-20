#!/bin/sh
# Regenerate src/lib/flipperpb/gen from src/lib/flipperpb/proto with nanopb
# (issue #388).  Needs `pip install nanopb==0.4.9.1` (it brings protoc via
# grpcio-tools: pip install grpcio-tools).  The output is committed; run this only after changing
# proto/ or the nanopb version, and check that version.h still matches
# proto/Changelog.
set -e
cd "$(dirname "$0")/../../src/lib/flipperpb"
v=$(python3 -c 'import nanopb, importlib.metadata as m; print(m.version("nanopb"))')
[ "$v" = 0.4.9.1 ] || { echo "mkflipperpb: nanopb $v, want 0.4.9.1" >&2; exit 1; }
top=$(sed -n 's/^## \[\([0-9]*\.[0-9]*\)\]$/\1/p' proto/Changelog | head -1)
want=$(sed -n 's/^#define FPB_MAJOR[^0-9]*\([0-9]*\)$/\1/p' version.h).$(sed -n 's/^#define FPB_MINOR[^0-9]*\([0-9]*\)$/\1/p' version.h)
[ "$top" = "$want" ] || { echo "mkflipperpb: Changelog says $top, version.h $want" >&2; exit 1; }
rm -f gen/*.pb.[ch]
cd proto
python3 -m nanopb.generator.nanopb_generator -q -D ../gen *.proto
echo "mkflipperpb: gen/ from protobuf $top"

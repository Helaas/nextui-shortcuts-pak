#!/bin/bash
set -euo pipefail

repo=$(cd "$(dirname "$0")/.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$tmp/bin" "$tmp/src"
cp "$repo/Makefile" "$tmp/Makefile"

# Exercise a fresh checkout without networking or Docker: the submodule must
# be initialized before the cross-build can read its headers.
cat > "$tmp/bin/git" <<'EOF'
#!/bin/bash
set -eu
test "$*" = 'submodule update --init'
mkdir -p third_party/apostrophe/include
touch third_party/apostrophe/include/apostrophe.h
EOF
cat > "$tmp/bin/docker" <<'EOF'
#!/bin/bash
test -f third_party/apostrophe/include/apostrophe.h
EOF
cat > "$tmp/bin/adb" <<'EOF'
#!/bin/bash
echo "$3" >> "$ADB_CALLS"
test "$3" != "$ADB_FAIL"
EOF
chmod +x "$tmp/bin/"*
export PATH="$tmp/bin:$PATH"
make -s -C "$tmp" universal
test -f "$tmp/third_party/apostrophe/include/apostrophe.h"

# Neither a failed remote setup nor a failed transfer may report success.
export ADB_CALLS="$tmp/adb-calls"
for ADB_FAIL in shell push; do
    export ADB_FAIL
    : > "$ADB_CALLS"
    if make -s -C "$tmp" deploy-platform MAKE=true \
        PLATFORM=h700 SERIAL=test > "$tmp/deploy.log" 2>&1; then
        echo "FAIL: deployment succeeded after adb $ADB_FAIL failed" >&2
        exit 1
    fi
    if grep -q 'Deploy complete' "$tmp/deploy.log"; then
        echo 'FAIL: deployment printed a false success message' >&2
        exit 1
    fi
    if [ "$ADB_FAIL" = shell ]; then
        test "$(cat "$ADB_CALLS")" = shell
    else
        test "$(cat "$ADB_CALLS")" = "$(printf 'shell\npush')"
    fi
done
echo 'PASS: fresh-checkout build and ADB deployment failure checks'

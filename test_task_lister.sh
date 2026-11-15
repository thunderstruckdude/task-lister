#!/usr/bin/env bash
# test_task_lister.sh — smoke-test suite for the task_lister kernel module
#
# Run as root (or with sudo) inside your Arch Linux VM.
# Usage:  sudo bash test_task_lister.sh
#
# The script:
#   1. Builds and loads the module
#   2. Checks that all /proc entries exist
#   3. Exercises flat list, tree, stats, and filter
#   4. Unloads the module and verifies cleanup

set -euo pipefail

MODULE="task_lister"
KO="task_lister.ko"
PROC_DIR="/proc/task_lister"

RED='\033[0;31m'
GRN='\033[0;32m'
YLW='\033[1;33m'
RST='\033[0m'

pass() { echo -e "${GRN}[PASS]${RST} $*"; }
fail() { echo -e "${RED}[FAIL]${RST} $*"; exit 1; }
info() { echo -e "${YLW}[INFO]${RST} $*"; }

# ── 0. Pre-flight ─────────────────────────────────────────────────────────
[[ $EUID -eq 0 ]] || fail "Must run as root (sudo bash $0)"
[[ -f "$KO" ]]    || fail "$KO not found — run 'make' first"

# Unload any stale instance
rmmod "$MODULE" 2>/dev/null && info "Removed stale module" || true

# ── 1. Load ───────────────────────────────────────────────────────────────
info "Loading $KO ..."
insmod "$KO"
sleep 0.3

lsmod | grep -q "$MODULE" && pass "Module loaded" || fail "Module not in lsmod"

# ── 2. /proc entries exist ────────────────────────────────────────────────
for entry in tasks tree filter stats; do
    [[ -e "$PROC_DIR/$entry" ]] \
        && pass "/proc/task_lister/$entry exists" \
        || fail "/proc/task_lister/$entry missing"
done

# ── 3. Flat task list ─────────────────────────────────────────────────────
info "Reading flat task list ..."
LINES=$(cat "$PROC_DIR/tasks" | wc -l)
[[ $LINES -gt 5 ]] && pass "tasks has $LINES lines" || fail "tasks output too short ($LINES lines)"

# Check header columns are present
grep -q "PID" "$PROC_DIR/tasks"  && pass "tasks header present" || fail "tasks header missing"

# Check that init/systemd appears
grep -qE "systemd|init" "$PROC_DIR/tasks" && pass "init/systemd found in tasks" \
    || info "init/systemd not found (may appear as 'swapper' in some configs)"

# ── 4. Tree output ────────────────────────────────────────────────────────
info "Reading process tree ..."
TREE_LINES=$(cat "$PROC_DIR/tree" | wc -l)
[[ $TREE_LINES -gt 3 ]] && pass "tree has $TREE_LINES lines" || fail "tree output too short"

grep -q "pid=" "$PROC_DIR/tree" && pass "tree contains pid= fields" || fail "tree missing pid= fields"

# ── 5. Stats ──────────────────────────────────────────────────────────────
info "Reading stats ..."
cat "$PROC_DIR/stats"
grep -q "Total tasks" "$PROC_DIR/stats" && pass "stats contains 'Total tasks'" || fail "stats missing total"
grep -q "Total RSS"   "$PROC_DIR/stats" && pass "stats contains 'Total RSS'"   || fail "stats missing RSS"

# ── 6. Filter ─────────────────────────────────────────────────────────────
info "Testing filter: set to S (sleeping) ..."
echo S > "$PROC_DIR/filter"
FILTER_VAL=$(cat "$PROC_DIR/filter")
echo "$FILTER_VAL" | grep -q "S" && pass "Filter set to S" || fail "Filter read-back failed: $FILTER_VAL"

SLEEP_LINES=$(cat "$PROC_DIR/tasks" | grep -v "^[╔╠║╚]" | wc -l)
[[ $SLEEP_LINES -gt 0 ]] && pass "Filtered tasks returned $SLEEP_LINES rows" \
    || fail "No tasks returned with S filter"

info "Testing filter: clearing ..."
echo - > "$PROC_DIR/filter"
cat "$PROC_DIR/filter" | grep -q "none" && pass "Filter cleared" || fail "Filter not cleared"

info "Testing filter: invalid character (should return error) ..."
echo Q > "$PROC_DIR/filter" 2>&1 && fail "Invalid filter accepted" || pass "Invalid filter correctly rejected"

# ── 7. dmesg messages ────────────────────────────────────────────────────
info "Checking kernel log messages ..."
dmesg | tail -20 | grep -q "task_lister" && pass "Module messages visible in dmesg" \
    || fail "No task_lister messages in dmesg"

# ── 8. Unload ─────────────────────────────────────────────────────────────
info "Unloading module ..."
rmmod "$MODULE"
sleep 0.2

lsmod | grep -q "$MODULE" && fail "Module still loaded after rmmod" || pass "Module unloaded cleanly"
[[ -e "$PROC_DIR" ]] && fail "/proc/task_lister still exists after rmmod" \
    || pass "/proc/task_lister removed after rmmod"

echo ""
echo -e "${GRN}════════════════════════════════════${RST}"
echo -e "${GRN}  All tests passed! ✓${RST}"
echo -e "${GRN}════════════════════════════════════${RST}"

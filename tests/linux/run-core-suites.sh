#!/usr/bin/env bash
# Linux runner for the fork's Qt Core-only qmake suites (T059).
#
#   tests/linux/run-core-suites.sh [--provision] <suite>...
#
# Builds each named tests/<suite>.pro out of tree (in a temporary directory, never in the
# checkout) with Qt 6's qmake and make, runs the resulting binary, and prints
# LINUX_CORE_SUITES_PASSED as its LAST line only when every named suite built and exited 0.
# Otherwise it names the failing suite(s) and exits non-zero.
#
# A suite qualifies only when its .pro declares `QT += core testlib` and nothing else; any other
# Qt module (network, gui, qml, websockets ...) is refused with exit 2, because this runner is
# evidence for pure Qt Core logic only. tests/linux/LINUX-SUITES.md records which suites pass.
#
# Exit codes: 0 all passed; 1 a suite failed to build or run; 2 bad argument or non-qualifying
# suite; 64 no suite named; 70 no Qt 6 toolchain (and --provision not given, or provisioning did
# not produce one).
#
# --provision installs ONLY Ubuntu's qt6-base-dev and qt6-base-dev-tools, and only when no Qt 6
# qmake is found. Ubuntu's Qt is the distribution version, not the Windows 6.11.2 build.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TESTS_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

PROVISION=0
SUITES=()
for arg in "$@"; do
    case "$arg" in
        --provision) PROVISION=1 ;;
        -h|--help)
            sed -n '2,18p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
            exit 0 ;;
        -*) echo "run-core-suites: unknown option: $arg" >&2; exit 2 ;;
        *) SUITES+=("$arg") ;;
    esac
done

if [ "${#SUITES[@]}" -eq 0 ]; then
    echo "usage: run-core-suites.sh [--provision] <suite>..." >&2
    exit 64
fi

# --- find a Qt 6 qmake ---------------------------------------------------------------------------
find_qmake() {
    local candidate version
    for candidate in qmake6 qmake; do
        if command -v "$candidate" >/dev/null 2>&1; then
            version="$("$candidate" -query QT_VERSION 2>/dev/null || true)"
            case "$version" in
                6.*) command -v "$candidate"; return 0 ;;
            esac
        fi
    done
    return 1
}

QMAKE=""
if ! QMAKE="$(find_qmake)"; then
    if [ "$PROVISION" -eq 1 ]; then
        echo "run-core-suites: no Qt 6 qmake found; provisioning qt6-base-dev and qt6-base-dev-tools"
        SUDO=""
        if [ "$(id -u)" -ne 0 ]; then
            if command -v sudo >/dev/null 2>&1; then SUDO="sudo"; else
                echo "run-core-suites: not root and no sudo; cannot provision" >&2
                exit 70
            fi
        fi
        $SUDO apt-get update -qq || echo "run-core-suites: apt-get update failed; trying install anyway" >&2
        $SUDO env DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
            qt6-base-dev qt6-base-dev-tools
        QMAKE="$(find_qmake)" || {
            echo "run-core-suites: provisioning finished but no Qt 6 qmake is on PATH" >&2
            exit 70
        }
    else
        echo "run-core-suites: no Qt 6 qmake (qmake6, or qmake reporting Qt 6) on PATH." >&2
        echo "run-core-suites: re-run with --provision (installs qt6-base-dev and qt6-base-dev-tools via apt-get)." >&2
        exit 70
    fi
fi

for tool in make g++; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "run-core-suites: '$tool' not found; install build-essential and make (not done by --provision)" >&2
        exit 70
    fi
done

QT_VERSION="$("$QMAKE" -query QT_VERSION)"
echo "run-core-suites: qmake=$QMAKE Qt=$QT_VERSION"

# --- validate suites before building anything ----------------------------------------------------
# Qualifying means the project's QT assignments name core and testlib and nothing else.
qualifies() {
    local pro="$1" line rest word found=0
    while IFS= read -r line; do
        line="${line%%#*}"
        case "$line" in
            *QT*=*) ;;
            *) continue ;;
        esac
        # Only plain `QT +=`, `QT =`, `QT -=` lines (not QT_ANYTHING, not conditional blocks).
        if [[ "$line" =~ ^[[:space:]]*QT[[:space:]]*(\+=|=)[[:space:]]*(.*)$ ]]; then
            found=1
            rest="${BASH_REMATCH[2]}"
            for word in $rest; do
                case "$word" in
                    core|testlib) ;;
                    *) return 1 ;;
                esac
            done
        fi
    done < "$pro"
    [ "$found" -eq 1 ]
}

for suite in "${SUITES[@]}"; do
    if [[ ! "$suite" =~ ^tst_[A-Za-z0-9_]+$ ]]; then
        echo "run-core-suites: not a suite name (expected tst_<name>): $suite" >&2
        exit 2
    fi
    if [ ! -f "$TESTS_DIR/$suite.pro" ]; then
        echo "run-core-suites: no such suite project: tests/$suite.pro" >&2
        exit 2
    fi
    if ! qualifies "$TESTS_DIR/$suite.pro"; then
        echo "run-core-suites: $suite does not qualify: its .pro declares Qt modules beyond 'core testlib'" >&2
        exit 2
    fi
done

# --- build and run each suite out of tree --------------------------------------------------------
WORK="$(mktemp -d "${TMPDIR:-/tmp}/seathub-core-suites.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

# QtTest links QtGui; there is no display in a cloud container, and a suite must not touch the
# host's real user directories.
export QT_QPA_PLATFORM="${QT_QPA_PLATFORM:-offscreen}"
export XDG_DATA_HOME="$WORK/xdg/data"
export XDG_CONFIG_HOME="$WORK/xdg/config"
export XDG_CACHE_HOME="$WORK/xdg/cache"
mkdir -p "$XDG_DATA_HOME" "$XDG_CONFIG_HOME" "$XDG_CACHE_HOME"

JOBS="$(nproc 2>/dev/null || echo 2)"
RUN_TIMEOUT=()
if command -v timeout >/dev/null 2>&1; then RUN_TIMEOUT=(timeout 600); fi

FAILED=()
for suite in "${SUITES[@]}"; do
    dir="$WORK/$suite"
    mkdir -p "$dir"
    echo "== $suite: build"
    if ! ( cd "$dir" && "$QMAKE" "$TESTS_DIR/$suite.pro" >qmake.log 2>&1 && make -j"$JOBS" >make.log 2>&1 ); then
        echo "== $suite: BUILD FAILED"
        tail -n 15 "$dir/qmake.log" "$dir/make.log" 2>/dev/null || true
        FAILED+=("$suite")
        continue
    fi
    if [ ! -x "$dir/$suite" ]; then
        echo "== $suite: BUILD FAILED (no binary $suite produced)"
        FAILED+=("$suite")
        continue
    fi
    echo "== $suite: run"
    set +e
    ( cd "$dir" && "${RUN_TIMEOUT[@]}" "./$suite" >run.log 2>&1 )
    status=$?
    set -e
    grep -E '^Totals:' "$dir/run.log" || true
    if [ "$status" -ne 0 ]; then
        echo "== $suite: FAILED (exit $status)"
        grep -E '^(FAIL!|XPASS)' "$dir/run.log" | head -n 20 || true
        tail -n 5 "$dir/run.log" || true
        FAILED+=("$suite")
    else
        echo "== $suite: passed"
    fi
done

if [ "${#FAILED[@]}" -gt 0 ]; then
    echo "LINUX_CORE_SUITES_FAILED: ${FAILED[*]}"
    exit 1
fi
echo "LINUX_CORE_SUITES_PASSED"

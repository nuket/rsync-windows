#!/bin/bash
#
# setup-linux.sh -- install everything needed to build rsync on Ubuntu.
#
# Tested on Ubuntu 22.04.5 LTS and 26.04 LTS.  Covers both build systems:
# the autoconf one (./configure && make) and the CMake one (CMakeLists.txt),
# plus the optional feature libraries and the manpage generator.
#
# Usage:
#     ./setup-linux.sh              # install required + optional
#     ./setup-linux.sh --minimal    # required only (no ACLs/xattrs/zstd/...)
#     ./setup-linux.sh --dry-run    # show what would be installed
#
# Safe to re-run: apt skips anything already present.
#
# Distributed under the same GPL-3.0-or-later terms as the rest of rsync.

set -uo pipefail

MINIMAL=0
DRY_RUN=0

for arg in "$@"; do
    case "$arg" in
    --minimal)  MINIMAL=1 ;;
    --dry-run)  DRY_RUN=1 ;;
    -h|--help)
        sed -n '2,17p' "$0" | sed 's/^# \{0,1\}//'
        exit 0 ;;
    *)
        echo "setup-linux.sh: unknown option '$arg' (try --help)" >&2
        exit 2 ;;
    esac
done

# ---------------------------------------------------------------- helpers

red()   { printf '\033[31m%s\033[0m\n' "$*"; }
green() { printf '\033[32m%s\033[0m\n' "$*"; }
bold()  { printf '\033[1m%s\033[0m\n' "$*"; }

die() { red "ERROR: $*"; exit 1; }

# ------------------------------------------------------- sanity checks

command -v apt-get >/dev/null 2>&1 \
    || die "no apt-get found -- this script is for Debian/Ubuntu systems."

DISTRO="unknown"
RELEASE="unknown"
if [ -r /etc/os-release ]; then
    # shellcheck disable=SC1091  # runtime file, not available to the linter
    . /etc/os-release
    DISTRO="${ID:-unknown}"
    RELEASE="${VERSION_ID:-unknown}"
fi

bold "rsync build dependencies"
echo "  distro ......... ${PRETTY_NAME:-$DISTRO $RELEASE}"
echo "  architecture ... $(uname -m)"

case "$DISTRO" in
ubuntu|debian|linuxmint|pop) ;;
*)  echo
    red "Warning: this script targets Ubuntu/Debian; '$DISTRO' is untested."
    echo "Continuing anyway -- package names may differ."
    ;;
esac

# apt needs root.  Re-exec under sudo rather than sprinkling it around, so
# that a single password prompt covers the whole run.
if [ "$(id -u)" -ne 0 ] && [ "$DRY_RUN" -eq 0 ]; then
    command -v sudo >/dev/null 2>&1 \
        || die "not running as root and sudo is not installed."
    echo
    echo "Re-running under sudo..."
    # Plain sudo, not "sudo -E": some sudoers configs refuse to preserve the
    # environment, and the exports below happen in the root instance anyway.
    exec sudo "$0" "$@"
fi

export DEBIAN_FRONTEND=noninteractive
export NEEDRESTART_MODE=a          # don't prompt about restarting services

# ------------------------------------------------------------- packages

# Needed to build rsync at all.
REQUIRED=(
    build-essential     # gcc, g++, make, libc headers
    gawk                # rsync needs a modern awk for its code generators
    autoconf            # ./configure is generated from configure.ac
    automake
    python3             # manpage + header generators, and the test suite
    cmake               # the CMake build
    ninja-build         # ...and its default generator
    git                 # version stamping (git-version.h)
    pkg-config
)

# Optional: each one switches on an rsync feature.  Missing ones only mean
# a smaller feature set, never a failed build.
OPTIONAL=(
    acl libacl1-dev         # --acls  (helper tools also used by the tests)
    attr libattr1-dev       # --xattrs (likewise)
    libxxhash-dev           # xxhash checksums (becomes the default)
    libzstd-dev             # zstd compression (becomes the default)
    liblz4-dev              # lz4 compression
    libssl-dev              # OpenSSL MD4/MD5
    zlib1g-dev              # for cmake -DRSYNC_EXTERNAL_ZLIB=ON
    libpopt-dev             # for ./configure --with-included-popt=no
)

# Development tooling: not needed to build rsync, but useful when working on
# its shell scripts -- including this one, which shellcheck keeps honest.
DEVTOOLS=(
    shellcheck
)
OPTIONAL+=("${DEVTOOLS[@]}")

# The manpages need one of two python3 markdown libraries; upstream prefers
# cmarkgfm.  Pick whichever this release actually offers.
MARKDOWN_PKG=""
for p in python3-cmarkgfm python3-commonmark; do
    cand=$(apt-cache policy "$p" 2>/dev/null | awk '/Candidate:/{print $2}')
    if [ -n "$cand" ] && [ "$cand" != "(none)" ]; then
        MARKDOWN_PKG="$p"
        break
    fi
done
if [ -n "$MARKDOWN_PKG" ]; then
    OPTIONAL+=("$MARKDOWN_PKG")
fi

if [ "$MINIMAL" -eq 1 ]; then
    OPTIONAL=()
fi

if [ "$DRY_RUN" -eq 1 ]; then
    echo
    bold "Would install (required):"
    printf '  %s\n' "${REQUIRED[@]}"
    if [ ${#OPTIONAL[@]} -gt 0 ]; then
        bold "Would install (optional):"
        printf '  %s\n' "${OPTIONAL[@]}"
    fi
    exit 0
fi

# ------------------------------------------------------------- install

echo
bold "Updating package lists..."
if ! apt-get update -qq; then
    red "apt-get update failed -- continuing with the cached lists."
fi

echo
bold "Installing required packages..."
if ! apt-get install -y "${REQUIRED[@]}"; then
    # Fall back to one-at-a-time so the failing package is obvious.
    red "Bulk install failed; retrying individually to find the culprit..."
    FAILED=()
    for p in "${REQUIRED[@]}"; do
        apt-get install -y "$p" || FAILED+=("$p")
    done
    [ ${#FAILED[@]} -eq 0 ] || die "could not install: ${FAILED[*]}"
fi

OPT_FAILED=()
if [ ${#OPTIONAL[@]} -gt 0 ]; then
    echo
    bold "Installing optional packages..."
    if ! apt-get install -y "${OPTIONAL[@]}"; then
        red "Bulk install failed; retrying individually..."
        for p in "${OPTIONAL[@]}"; do
            apt-get install -y "$p" || OPT_FAILED+=("$p")
        done
    fi
fi

# --------------------------------------------------------------- verify

echo
bold "Verifying the toolchain..."

MISSING=()
check_cmd() {
    if command -v "$1" >/dev/null 2>&1; then
        printf '  %-16s %s\n' "$1" "$(command -v "$1")"
    else
        printf '  %-16s %s\n' "$1" "MISSING"
        MISSING+=("$1")
    fi
}

for c in gcc g++ make gawk autoconf automake cmake ninja python3 git; do
    check_cmd "$c"
done

# Not required to build, so report it without failing the run.
if command -v shellcheck >/dev/null 2>&1; then
    printf '  %-16s %s\n' "shellcheck" "$(command -v shellcheck)"
else
    printf '  %-16s %s\n' "shellcheck" "not installed (optional)"
fi

echo
bold "Verifying optional libraries..."
check_header() {
    # $1 = human name, $2 = header path
    if [ -e "/usr/include/$2" ] || \
       find /usr/include -maxdepth 3 -name "$(basename "$2")" -print -quit \
            2>/dev/null | grep -q .; then
        printf '  %-16s yes\n' "$1"
    else
        printf '  %-16s no\n' "$1"
    fi
}
check_header acl        sys/acl.h
check_header xattr      sys/xattr.h
check_header xxhash     xxhash.h
check_header zstd       zstd.h
check_header lz4        lz4.h
check_header openssl    openssl/md5.h
check_header zlib       zlib.h

echo
bold "Verifying the manpage generator..."
MD_OK=0
for m in cmarkgfm commonmark; do
    if python3 -c "import $m" >/dev/null 2>&1; then
        echo "  python3 module '$m' importable"
        MD_OK=1
        break
    fi
done
if [ "$MD_OK" -eq 0 ]; then
    if [ "$MINIMAL" -eq 1 ]; then
        echo "  skipped (--minimal); build with ./configure --disable-md2man"
    else
        red "  neither cmarkgfm nor commonmark is importable."
        echo "  Install one with:  python3 -mpip install --user commonmark"
        echo "  ...or build with:  ./configure --disable-md2man"
    fi
fi

# ---------------------------------------------------------------- done

echo
if [ ${#MISSING[@]} -ne 0 ]; then
    red "Missing required tools: ${MISSING[*]}"
    exit 1
fi

if [ ${#OPT_FAILED[@]} -ne 0 ]; then
    red "Optional packages that failed to install: ${OPT_FAILED[*]}"
    echo "The build will still work, with those features disabled."
fi

green "All required build dependencies are installed."
cat <<'EOF'

Build rsync with either build system:

  autoconf:   ./configure && make
  CMake:      cmake -B build -G Ninja && cmake --build build

Run the test suite (needs the autoconf build's helper programs):

  make check
EOF

#!/usr/bin/env bash
# -----------------------------------------------------------------------------
# macos_bundle_deps.sh
#
# Turn a macOS .app bundle produced by the idiff build into a self-contained
# bundle that no longer depends on Homebrew (/opt/homebrew, /usr/local) or
# any other non-system prefix on the build machine.
#
# What it does
# ------------
# 1. Walks every Mach-O file (executable + dylib) inside the bundle and
#    collects every LC_LOAD_DYLIB entry that points into a "relocatable"
#    prefix (/opt/homebrew, /usr/local, or any path passed via --extra-prefix).
# 2. Copies each such dylib into <bundle>/Contents/Frameworks/ (flat layout).
#    Symlinks are resolved so we always copy the real file.
# 3. Rewrites the LC_ID_DYLIB of every copied dylib to @rpath/<basename>.
# 4. Rewrites every LC_LOAD_DYLIB reference (in the main executable and in
#    the copied dylibs) that points at a relocatable prefix so it now reads
#    @rpath/<basename>.
# 5. Adds @executable_path/../Frameworks to the main executable's LC_RPATH
#    (idempotent).
# 6. Ad-hoc re-signs every modified Mach-O so dyld is willing to load it on
#    Apple Silicon (hardened-runtime policy requires a valid signature after
#    any load-command change).
#
# Why a custom script instead of CMake's fixup_bundle / GET_RUNTIME_DEPENDENCIES
# -----------------------------------------------------------------------------
# - fixup_bundle() is a closed box and silently drops dylibs whose install
#   name doesn't match their on-disk path (a common pattern on Homebrew).
# - GET_RUNTIME_DEPENDENCIES can't follow @rpath entries through
#   @loader_path-based Homebrew bottles reliably across versions.
# - A shell script that delegates to `otool` + `install_name_tool` + `codesign`
#   is ~100 lines, trivial to debug, and easy to reason about.
#
# Usage
# -----
#   macos_bundle_deps.sh <path-to-app-bundle>
#       [--extra-prefix /some/other/prefix]...
#
# Exit status
# -----------
#   0 on success, non-zero on any failure.
#
# Assumptions
# -----------
# - Runs on macOS with the standard developer-tools command line.
# - Dependencies that live under /System, /usr/lib or /Library/Frameworks
#   are treated as system and left alone.
# -----------------------------------------------------------------------------

set -euo pipefail

if [[ $# -lt 1 ]]; then
    echo "Usage: $0 <path-to-app-bundle> [--extra-prefix PATH]..." >&2
    exit 2
fi

APP="$1"; shift

# Default relocatable prefixes: anything under Homebrew on Apple Silicon
# or Intel, plus anything a user explicitly opts-in to.
RELOCATABLE_PREFIXES=(/opt/homebrew /usr/local/opt /usr/local/Cellar /usr/local/lib)

while [[ $# -gt 0 ]]; do
    case "$1" in
        --extra-prefix)
            RELOCATABLE_PREFIXES+=("$2")
            shift 2
            ;;
        *)
            echo "Unknown argument: $1" >&2
            exit 2
            ;;
    esac
done

if [[ ! -d "$APP" ]]; then
    echo "Not a directory: $APP" >&2
    exit 1
fi

MACOS_DIR="$APP/Contents/MacOS"
FW_DIR="$APP/Contents/Frameworks"
mkdir -p "$FW_DIR"

# Resolve the main executable from the bundle's Info.plist so this script
# keeps working if the target is ever renamed.
EXE_NAME=$(/usr/libexec/PlistBuddy -c "Print :CFBundleExecutable" \
    "$APP/Contents/Info.plist" 2>/dev/null \
    || basename "$(ls "$MACOS_DIR" | head -n1)")
MAIN_EXE="$MACOS_DIR/$EXE_NAME"

if [[ ! -f "$MAIN_EXE" ]]; then
    echo "Main executable not found: $MAIN_EXE" >&2
    exit 1
fi

is_relocatable() {
    local path="$1"
    local p
    for p in "${RELOCATABLE_PREFIXES[@]}"; do
        [[ "$path" == "$p"/* ]] && return 0
    done
    return 1
}

# List LC_LOAD_DYLIB / LC_LOAD_WEAK_DYLIB entries (skip the LC_ID_DYLIB of
# the file itself, which `otool -L` prints as line 2 for dylibs).
list_deps() {
    local file="$1"
    # `otool -L` format: "\t<path> (compatibility...)"; skip header, skip self-id.
    local kind
    kind=$(file -b "$file")
    if [[ "$kind" == *"dynamically linked shared library"* ]]; then
        otool -L "$file" | awk 'NR>2 {print $1}'
    else
        otool -L "$file" | awk 'NR>1 {print $1}'
    fi
}

# BFS through the dependency graph. We avoid `declare -A` (bash 4+) and
# bash arrays of arbitrary length so this script works under the macOS
# system bash 3.2 as well.
#
# Each QUEUE entry is "<bundle-path>|<source-path>" where:
#   bundle-path: the file inside the .app we read/modify
#   source-path: the original on-disk path (Homebrew), needed to
#                resolve @loader_path references against the *original*
#                directory rather than Contents/Frameworks.
# The main executable's bundle-path and source-path are the same.
#
# COPIED_LIST is a newline-separated list of basenames already bundled,
# used as an "is present?" set via `grep -Fx`.
QUEUE="$MAIN_EXE|$MAIN_EXE"
COPIED_LIST=""

while [[ -n "$QUEUE" ]]; do
    entry="$(printf '%s\n' "$QUEUE" | head -n1)"
    QUEUE="$(printf '%s\n' "$QUEUE" | tail -n +2)"

    # Skip blank entries that can creep in from our newline-concatenation
    # style of queue building.
    [[ -z "$entry" ]] && continue

    current="${entry%%|*}"
    source="${entry#*|}"

    # Snapshot this file's LC_RPATH entries *before* we mutate anything,
    # so we can resolve @rpath/foo.dylib references in its LC_LOAD_DYLIB
    # entries against the rpaths the upstream bottle built with. These
    # rpaths typically point into Homebrew (.../lib) and get stripped
    # later in this same iteration.
    RPATHS="$(otool -l "$current" \
              | awk '/cmd LC_RPATH/{getline; getline; print $2}')"

    while IFS= read -r dep; do
        [[ -z "$dep" ]] && continue

        # Resolve @rpath / @loader_path / @executable_path references to
        # an on-disk path so is_relocatable() and the copy step can work
        # uniformly. @loader_path is resolved against the *source*
        # location, not the bundle copy, because the bottle's rpath graph
        # is only meaningful in its original filesystem layout.
        resolved="$dep"
        case "$dep" in
            @rpath/*)
                suffix="${dep#@rpath/}"
                resolved=""
                while IFS= read -r rp; do
                    [[ -z "$rp" ]] && continue
                    case "$rp" in
                        @loader_path*) rp="$(dirname "$source")${rp#@loader_path}";;
                        @executable_path*) rp="$(dirname "$MAIN_EXE")${rp#@executable_path}";;
                    esac
                    if [[ -f "$rp/$suffix" ]]; then
                        resolved="$rp/$suffix"
                        break
                    fi
                done <<< "$RPATHS"
                [[ -z "$resolved" ]] && continue
                ;;
            @loader_path/*)
                resolved="$(dirname "$source")/${dep#@loader_path/}"
                ;;
            @executable_path/*)
                resolved="$(dirname "$MAIN_EXE")/${dep#@executable_path/}"
                ;;
        esac

        if ! is_relocatable "$resolved"; then
            continue
        fi

        # Resolve symlinks so we always copy the real dylib. macOS has no
        # GNU `readlink -f`, so fall back to a short Perl one-liner.
        real=$(/usr/bin/perl -MCwd -e 'print Cwd::abs_path(shift)' "$resolved")
        # Use the basename of the *reference* (dep) so the copied file's
        # name matches the install name its sibling dylibs were linked
        # against. Homebrew ships symlinks for this exact purpose, e.g.
        #   libopencv_imgproc.413.dylib -> libopencv_imgproc.4.13.0.dylib
        base=$(basename "$dep")

        if ! printf '%s\n' "$COPIED_LIST" | grep -Fxq "$base"; then
            cp -f "$real" "$FW_DIR/$base"
            chmod u+w "$FW_DIR/$base"
            # Canonicalise its own LC_ID_DYLIB.
            install_name_tool -id "@rpath/$base" "$FW_DIR/$base"
            COPIED_LIST="$COPIED_LIST
$base"
            # Enqueue with the *source* location we resolved to, so the
            # next iteration can expand @loader_path correctly.
            QUEUE="$QUEUE
$FW_DIR/$base|$real"
        fi

        # Rewrite the reference in the file that pulled it in. If dep is
        # already @rpath/<base> the -change is a no-op but still succeeds.
        install_name_tool -change "$dep" "@rpath/$base" "$current"
    done < <(list_deps "$current")

    # Strip any LC_RPATH entries that point into a relocatable prefix.
    # Homebrew bottles commonly embed cellar / opt paths as rpath, which
    # would let dyld fall back to the build machine's libraries at runtime
    # and defeat the whole point of this script. Also strip rpaths whose
    # @loader_path expansion doesn't resolve inside the bundle -- those
    # only made sense in the source tree.
    while IFS= read -r rp; do
        [[ -z "$rp" ]] && continue
        if is_relocatable "$rp"; then
            install_name_tool -delete_rpath "$rp" "$current" 2>/dev/null || true
        elif [[ "$rp" == @loader_path* ]]; then
            # @loader_path-based rpaths from Homebrew bottles (e.g.
            # libwebp's "@loader_path/../lib") point outside the bundle
            # and will produce "tried: .../../lib/..." failures at
            # runtime. We've already bundled every needed dylib into
            # Contents/Frameworks, so drop them.
            install_name_tool -delete_rpath "$rp" "$current" 2>/dev/null || true
        fi
    done <<< "$RPATHS"
done

# Make sure the main executable can find @rpath/*. Adding the same rpath
# twice is an error, so we check first.
if ! otool -l "$MAIN_EXE" | grep -q "path @executable_path/../Frameworks "; then
    install_name_tool -add_rpath "@executable_path/../Frameworks" "$MAIN_EXE"
fi

# dyld on Apple Silicon refuses to load a Mach-O whose signature is stale
# with respect to its load commands. Ad-hoc re-sign everything we touched.
codesign --force --sign - "$MAIN_EXE"
for f in "$FW_DIR"/*.dylib; do
    [[ -f "$f" ]] && codesign --force --sign - "$f"
done

echo "Bundled $(ls "$FW_DIR" | wc -l | tr -d ' ') dylib(s) into $FW_DIR"

# Shared helpers, sourced by every phase script.  Not run directly.

log()  { printf '\n\033[1;36m[%s]\033[0m %s\n' "$(date +%H:%M:%S)" "$*"; }
ok()   { printf '\033[1;32m  OK  %s\033[0m\n' "$*"; }
warn() { printf '\033[1;33m  !!  %s\033[0m\n' "$*" >&2; }
die()  { printf '\033[1;31m  XX  %s\033[0m\n' "$*" >&2; exit 1; }

# Ensure we are in the MINGW64 toolchain and export the build env.
require_mingw() {
  [ -x "$MINGW/bin/gcc.exe" ] || die "mingw64 gcc not found at $MINGW/bin. Open the MSYS2 *MINGW64* terminal (prompt shows MINGW64, not MSYS)."
  export PATH="$MINGW/bin:$PATH"          # mingw64 first — also beats Strawberry Perl
  export LLVM_INSTALL_DIR="C:/msys64/mingw64"   # shiboken6 needs this
  export QT_API="pyside6"                 # helps qtpy pick the Qt6 binding
}

# Parallelism.
jobs_n()        { if [ "${JOBS:-auto}" = "auto" ]; then nproc; else printf '%s' "$JOBS"; fi; }
natron_jobs_n() { local j; j="$(jobs_n)"
  if [ -n "${NATRON_JOBS:-}" ]; then printf '%s' "$NATRON_JOBS"
  elif [ "$j" -gt 8 ]; then printf '8'           # cap moc/autogen race exposure
  else printf '%s' "$j"; fi; }

# Detect the mingw64 python X.Y (the stdlib dir name changes across MSYS2 updates).
py_ver() { "$MINGW/bin/python3.exe" -c 'import sys;print("%d.%d"%sys.version_info[:2])'; }

#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'USAGE'
Usage: tools/check_install_runtime_destinations.sh [cmake-file...]

Validate that install(TARGETS ...) rules with LIBRARY/ARCHIVE destinations
also specify a RUNTIME destination (needed for DLL installs on Windows).

When no files are passed, scans tracked CMake files in the repository,
excluding src/third_party/.
USAGE
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
  usage
  exit 0
fi

repo_root="$(git rev-parse --show-toplevel 2>/dev/null || pwd)"
cd "$repo_root"

cmake_files=()
if [[ $# -gt 0 ]]; then
  cmake_files=("$@")
else
  if git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    while IFS= read -r f; do
      case "$f" in
        src/third_party/*) continue ;;
      esac
      cmake_files+=("$f")
    done < <(git ls-files -- 'CMakeLists.txt' '*.cmake')
  else
    while IFS= read -r f; do
      case "$f" in
        src/third_party/*) continue ;;
      esac
      cmake_files+=("$f")
    done < <(find . -type f \( -name 'CMakeLists.txt' -o -name '*.cmake' \) | sed 's#^\./##' | sort)
  fi
fi

if [[ ${#cmake_files[@]} -eq 0 ]]; then
  echo "No CMake files found to scan."
  exit 0
fi

if ! command -v perl >/dev/null 2>&1; then
  echo "perl not found in PATH; required for install-rule validation." >&2
  exit 1
fi

set +e
perl - "${cmake_files[@]}" <<'PERL'
use strict;
use warnings;

my @files = @ARGV;
my $scanned = 0;
my $violations = 0;

for my $file (@files) {
    next unless -f $file;
    open my $fh, '<', $file or die "Unable to read $file: $!\n";
    local $/;
    my $text = <$fh>;
    close $fh;
    $scanned++;

    pos($text) = 0;
    while ($text =~ /^[ \t]*install\s*\(/mig) {
        my $cmd_start = $-[0];
        my $open_idx = $+[0] - 1; # index of '('
        my $depth = 0;
        my $end_idx = -1;

        for (my $i = $open_idx; $i < length($text); $i++) {
            my $ch = substr($text, $i, 1);
            $depth++ if $ch eq '(';
            $depth-- if $ch eq ')';
            if ($depth == 0) {
                $end_idx = $i;
                last;
            }
        }

        if ($end_idx < 0) {
            my $line = 1 + (substr($text, 0, $cmd_start) =~ tr/\n//);
            print STDERR "$file:$line: unterminated install(...) command.\n";
            $violations++;
            last;
        }

        my $body = substr($text, $open_idx + 1, $end_idx - $open_idx - 1);
        pos($text) = $end_idx + 1;

        next unless $body =~ /^\s*TARGETS\b/is;

        my $has_library = $body =~ /\bLIBRARY\s+DESTINATION\b/is;
        my $has_archive = $body =~ /\bARCHIVE\s+DESTINATION\b/is;
        my $has_runtime = $body =~ /\bRUNTIME\s+DESTINATION\b/is;

        next unless ($has_library || $has_archive);
        next if $has_runtime;

        my $line = 1 + (substr($text, 0, $cmd_start) =~ tr/\n//);
        print STDERR "$file:$line: install(TARGETS ...) has LIBRARY/ARCHIVE DESTINATION but missing RUNTIME DESTINATION.\n";
        $violations++;
    }
}

if ($violations) {
    print STDERR "Found $violations install(TARGETS ...) block(s) missing RUNTIME DESTINATION.\n";
    exit 1;
}

print "Checked $scanned CMake file(s): install(TARGETS ...) runtime destinations look good.\n";
exit 0;
PERL
rc=$?
set -e

exit "$rc"

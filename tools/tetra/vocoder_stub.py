#!/usr/bin/env python3
"""
TETRA TCH/FS vocoder stub — integration-test placeholder.

Implements the synchronous, persistent-subprocess protocol used by dsd-neo:

    dsd-neo  ──► stdin : FRAME_BITS bytes (one byte per coded bit, value 0 or 1)
    dsd-neo  ◄── stdout: FRAME_SAMPLES × 2 bytes (PCM16LE @ 8 kHz, mono)
    (repeat per frame until dsd-neo closes the pipe)

This stub produces silence (all-zero PCM) for every frame, which lets you
verify the full audio-routing path without a real TETRA ACELP codec.

Usage (PowerShell):
    $env:TETRA_VOCODER_CMD = "python $PWD\\tools\\tetra\\vocoder_stub.py"

Usage (bash):
    export TETRA_VOCODER_CMD="python3 $(pwd)/tools/tetra/vocoder_stub.py"

Replace this stub with a real TETRA ACELP vocoder command to hear actual audio.
"""

import sys

FRAME_BITS    = 137   # coded bits per TCH/FS ACELP sub-frame
FRAME_SAMPLES = 160   # PCM samples per frame (20 ms @ 8 kHz)
PCM_SILENCE   = bytes(FRAME_SAMPLES * 2)  # pre-built silence payload


def main() -> None:
    stdin  = sys.stdin.buffer
    stdout = sys.stdout.buffer

    while True:
        data = stdin.read(FRAME_BITS)
        if not data:
            break                       # dsd-neo closed the pipe → exit cleanly
        if len(data) < FRAME_BITS:
            break                       # short read at EOF

        # --short-output exercises the decoder's IPC framing/error recovery.
        if "--short-output" in sys.argv[1:]:
            stdout.write(PCM_SILENCE[: len(PCM_SILENCE) // 2])
            stdout.flush()
            break

        # Output one frame of PCM16LE silence.
        stdout.write(PCM_SILENCE)
        stdout.flush()


if __name__ == "__main__":
    main()

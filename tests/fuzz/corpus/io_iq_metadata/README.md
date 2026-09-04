# IQ Metadata Fuzz Corpus

Files in this corpus are parser seeds for `dsd_iq_replay_read_metadata`.

`minimal.json` uses the current `format: "dsd-neo-iq"` metadata schema and is intended to exercise metadata parsing. It
does not include the referenced I/Q data file, so it is not a standalone replay fixture for `--iq-replay`.

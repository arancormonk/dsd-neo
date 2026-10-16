# WAV I/Q Fuzz Corpus

Seeds for `dsd_iq_wav_read_info` and the replay reader over a WAV I/Q recording (`dsd_iq_replay_open`,
`dsd_iq_replay_read`, `dsd_iq_replay_seek`). Each is a small 2-channel WAV made for this corpus:

- `pcm_s16.wav`: plain RIFF, 16-bit PCM.
- `pcm_s24_meta_auxi.wav`: 24-bit PCM at 48254 Hz with an odd-sized `meta` chunk and an SDR#-style `auxi` chunk.
- `float64_ext.wav`: WAVE_FORMAT_EXTENSIBLE 64-bit float, one sample infinite.
- `unfinalized.wav`: a streaming RIFF whose RIFF and data sizes were never written.
- `rf64.wav`: RF64 with `ds64` sizes and a trailing `LIST` chunk after the data.
- `fmt_after_data_u8.wav`: 8-bit PCM with `fmt` after `data`.
- `riff_ends_inside_data.wav`: a RIFF whose size ends the container 4 bytes into a data chunk that declares more.

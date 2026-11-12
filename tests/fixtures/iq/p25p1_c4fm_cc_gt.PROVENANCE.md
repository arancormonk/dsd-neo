# P25 Phase 1 C4FM real-air control-channel fixtures provenance

`p25p1_c4fm_cc_gt_s9.iq` and `p25p1_c4fm_cc_gt_city.iq` are converted from two
captures in [MattCheramie/GopherTrunk](https://github.com/MattCheramie/GopherTrunk)'s
test data, pinned at commit
[`f3798e04fa0498a54e969d2d13ec154c0ff0a65e`](https://github.com/MattCheramie/GopherTrunk/tree/f3798e04fa0498a54e969d2d13ec154c0ff0a65e).
They are redistributed under that repository's Apache License 2.0; the upstream
license text at that commit is preserved as `p25p1_c4fm_cc_gt.APACHE-2.0.txt`.

Upstream describes both as slices of RTL-SDR recordings of two sites of one P25
Phase 1 C4FM trunked system, made by the reporter of upstream issue #402 and
channelised upstream to a 48 kHz centred complex baseband in GNU Radio's
headerless little-endian float32 I/Q layout (`cfile`). Capture timestamps are
not provided upstream.

`mmr-s9-cc.cfile` (source of `p25p1_c4fm_cc_gt_s9.iq`):

- Upstream path: `cmd/gophertrunk/testdata/mmr-s9-cc.cfile`, last changed by commit
  [`2194316a8b8637c5aa2a98c1dc436fca1167f343`](https://github.com/MattCheramie/GopherTrunk/commit/2194316a8b8637c5aa2a98c1dc436fca1167f343)
- Git blob SHA-1: `cd803917bcde5f3db397772551ba6d75b1490487`
- Raw SHA-256: `b9e9097f80dc68e56d93d012a48e363f264bc6bf028419582727da0273596981`
- 419,432 bytes, 52,429 samples, about 1.09 s; published centre 420.0500 MHz from a
  2.4 MS/s recording
- Decoded here: NAC 0x164, WACN BEE00, SYS 164 (upstream's own note says NAC 0x167)

`mmr-city-cc.cfile` (source of `p25p1_c4fm_cc_gt_city.iq`):

- Upstream path: `cmd/gophertrunk/testdata/mmr-city-cc.cfile`, added by commit
  [`797c8f4af63e2b49c35706c08579d7b673c73749`](https://github.com/MattCheramie/GopherTrunk/commit/797c8f4af63e2b49c35706c08579d7b673c73749)
- Git blob SHA-1: `94782fe613a4c34311dfdd7dba1be0e6db66a228`
- Raw SHA-256: `deecc2aad19a8c84bd15e20346e9838264308b3a7ead45e6575ce21f854ca2b6`
- 576,000 bytes, 72,000 samples, 1.5 s; published centre 420.0125 MHz from an
  off-channel 960 kS/s recording
- Decoded here: NAC 0x164, WACN BEE00, SYS 164

The conversion is `tools/build_iq_fixtures.py` (`CF32_CAPTURES`), which downloads
each file from the pinned commit, checks its SHA-256 and requantizes the whole
capture to cu8 after scaling its largest I or Q component to 0.9 of full scale. The
rate, length and carrier are unchanged: the carriers stay about -51 Hz and -9 Hz
off centre for the receiver to recover. The sidecars are the script's, with the
published centre frequencies. Regenerate with:

```text
python3 tools/build_iq_fixtures.py --only p25p1_c4fm_cc_gt_s9 --only p25p1_c4fm_cc_gt_city
```

The SHA-256 of the committed cu8 files are
`84044545de1874cee3c610e33af0f43698cb42398781f990197f2fb6fc687568` (s9) and
`04e32d159051b0c6f0943ea5fe0ccf7b43f164e1202f2916354563c26df167b5` (city).
Under `-f1` the original float32 captures and the cu8 fixtures decode the same
number of TSBK messages, NACs and site identities (41, 14 and 8; 57, 20 and 18).

Both captures have an asymmetric C4FM eye: the four deviation levels sit at about
-1957/-694/+560/+1840 Hz (s9) and -1972/-690/+554/+1814 Hz (city), outer to inner
ratios about 3.3 and 2.8 against the nominal 3. The `DECODE_IQ_P25P1_C4FM_CC_GT_*`
cases in `tests/CMakeLists.txt` assert floors on the CRC-passed TSBK messages, the
NAC and the site identity, and that no voice frame appears.

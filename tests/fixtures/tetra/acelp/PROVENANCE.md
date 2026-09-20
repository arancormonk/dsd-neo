# TETRA “Hello Tetra” ACELP reference

`hello_tetra.acelp` is an unmodified copy of `testfile.acelp` from
[`sq5bpf/telive`](https://github.com/sq5bpf/telive). The upstream documentation
states that playing this sample should produce the phrase “Hello Tetra”.

- Repository commit: `ea873b55a786abee0de756e850c07555e4ffe734`
- Git blob: `0ba057bf906639d928fead0dcba087835adef0ff`
- Source file SHA-256: `83473499f59f95efdeacae6cd6b87fa6139b0c8982e24b952d512ece8d439348`
- License: GPL-3.0; the upstream license is preserved as `GPL-3.0.txt`

`hello_tetra.codec` contains the 60 decoder-input records produced from the
30 channel frames by the ETSI TETRA speech channel reference decoder. Its
SHA-256 is
`9534bfb74db896bb4dfa0bf2e48242cefeec7e6da5f7c0736ad0ca86407c65f0`.
The regression test compares all 8,220 ACELP bits with this reference.

`hello_tetra_reference.wav` is the corresponding mono PCM16 reference at
8 kHz: 14,400 samples (1.8 seconds), with SHA-256
`80f28d1f2acf147d5d8843affc7d952d088824e989eb70a2aa763db497794f85`.

The reference artifacts were generated locally with ETSI EN 300 395-2
V1.3.1 package `en_30039502v010301p0.zip` downloaded from ETSI. Package hashes:

- MD5: `a8115fe68ef8f8cc466f4192572a1e3e`
- SHA-256: `1fe18c4773c8ccb52ef23ca5b4a0b0841b38d54ddd5d9c7d86eb8b060f132f39`

The ETSI decoder source and binaries are not redistributed. The local decoder
was adapted using `sq5bpf/install-tetra-codec` commit
`bfd21ceb1159e8d5f12221a1570bd3da10e77832`. Decoding produced 60 speech
frames of 240 samples each. The raw PCM SHA-256 before WAV wrapping was
`d816a513c0bb759f8bf07f6ce5284b91050724fbb8b6184276f0905502831b90`.

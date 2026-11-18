# M17 Support Scope

DSD-neo tracks the current published M17 air-interface specification for frame layout, sync words, FEC stages, LSF
semantics, stream frames, packet frames, BERT frames, and EOT handling. The M17 UDP/IP input and output support is a
DSD-neo transport for M17 frames; it is separate from RF air-interface channel-access behavior.

Implemented decode coverage:

- LSF, stream, packet, BERT, and EOT sync handling.
- Current LSF TYPE field semantics, including stream/packet mode, data type, encryption type/subtype, CAN, and signed
  stream advertisement.
- M17 address classification for reserved, standard, extended, and destination-only broadcast addresses.
- Stream metadata for text, GNSS position, and extended callsign blocks, read only from an LSF (or a stream's LSD,
  or an IP stream or packet frame) whose CRC passed, and never from what the local encoder transmits. Under `-F` an
  LSF that failed its CRC still sets up the stream, but its META is not read. A stream's LSD counts only when all six
  of its LICH pieces were heard in one transmission, on one tuning, and on the same side of the moment the stream was
  confirmed; one put together across a retune or another change is dropped, and the next whole set is read.
- GNSS position META: the position goes to the call's History row, the position feed and the control API's `positions`
  topic, once per transmission, sender and META content. A coordinate of -2^23 (never used by the specification) is no position,
  a bearing above 359 degrees reads `Bearing: Invalid;` with the speed kept, and radius code 7 counts as "more than 64
  m". M17 positions are not written to `-L` (its source column is a number; M17 sends a callsign). Packets never carry a
  position: M17 defines no GNSS packet protocol, and a packet with protocol 0x91 is shown as an unknown protocol.
- Stream payload descrambling and AES-CTR decrypt when the corresponding key material is configured.
- Signed voice-stream collection and secp256r1 verification when `--m17-signature-public-key <hex>` is provided.
- Packet assembly with the current 823-byte application-payload ceiling and packet CRC validation.
- BERT PRBS9 payload checking without requiring a preceding LSF.

Implemented encode coverage:

- Local stream, packet, and BERT frame generation for test and airgap workflows.
- Current sync, scrambling, FEC, interleaving, and packet EOF metadata layout.

Not implemented:

DSD-neo does not implement M17 CSMA channel access or claim MAC/channel-access conformance. The M17 CSMA default
constants exposed by the helper algorithms are informational spec defaults only; they are not a transmitter state
machine, carrier-sense policy, or slot arbitration implementation.

The local M17 encoders do not currently generate encrypted streams or signed voice streams.

If channel-access behavior is added later, it should be designed and tested as transmitter policy rather than treated as
a side effect of the existing frame encoder.

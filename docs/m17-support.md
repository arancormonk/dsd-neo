# M17 Support Scope

DSD-neo tracks the current published M17 air-interface specification for frame layout, sync words, FEC stages, LSF
semantics, stream frames, packet frames, BERT frames, and EOT handling. The M17 UDP/IP input and output support is a
DSD-neo transport for M17 frames; it is separate from RF air-interface channel-access behavior.

Implemented decode coverage:

- LSF, stream, packet, BERT, and EOT sync handling. The end-of-transmission marker ends a call only once three of its
  words have been heard in a row, so a fade that leaves a flat run of symbols does not end the call; the rest of the
  marker is skipped up to the first symbols that are not part of it, so the next transmission is not cut.
- Current LSF TYPE field semantics, including stream/packet mode, data type, encryption type/subtype, CAN, and signed
  stream advertisement.
- M17 address classification for reserved, standard, extended, and destination-only broadcast addresses.
- Stream metadata for text, GNSS position, and extended callsign blocks, read only from an LSF (or a stream's LSD, or an
  IP stream or packet frame) whose CRC passed, and never from what the local encoder transmits. Under `-F` an LSF that
  failed its CRC still sets up the stream, but its META is not read, and once an LSF whose CRC passed has set up the
  transmission, one that failed it is only shown: it does not change the CAN, encryption or stream type. If the end of
  one transmission is missed and the next one's LSF fails its CRC, the previous transmission's CAN and encryption stay
  in force until the new stream's LSF is rebuilt from its LICH with a good CRC: one LICH cycle (six stream frames, 240
  ms) when those frames are received cleanly. A stream's LSD counts only when all six of its LICH pieces were heard in
  one transmission, on one tuning, and on the same side of the moment the stream was confirmed; one put together across
  a retune or another change is dropped, and the next whole set is read.
- GNSS position META: the position goes to the call's History row, the position feed and the control API's `positions`
  topic, once per transmission, sender and META content. The console prints its coordinates only once the position was
  accepted; a refused one (such as exactly 0, 0) prints `GPS: (refused);` with its other fields. A coordinate of -2^23
  (never used by the specification) is no position, a bearing above 359 degrees reads `Bearing: Invalid;` with the speed
  kept, and radius code 7 counts as "more than 64 m" (printed `Radius: >64 m;`, never 128 m). M17 positions are not
  written to `-L` (its source column is a number; M17 sends a callsign). Packets never carry a position: M17 defines no
  GNSS packet protocol, and a packet with protocol 0x91 is shown as an unknown protocol.
- Stream payload descrambling and AES-CTR decrypt when the corresponding key material is configured.
- M17 over UDP/IP: a network frame whose CRC failed, whose own link setup cannot be used, or that carries a packet-mode
  link setup in a stream frame, is dropped (not played, no call). Each frame of a stream (its stream ID and station) is
  used once and in order, through the frame counter's wrap: a frame received again, or after a later one, is dropped. A
  new stream ID or station ends the stream before it and starts its own call; a final frame of another stream does not
  end the open one. A stream's own end, or EOTX and DISC from its station, end it and drop the carrier. After a network
  stream ends, its stream ID (from that station) is ignored until a frame 0 starts it again, however many other streams
  end meanwhile; a stream ID that the same station reuses with no end received between is not taken.
- Signed voice streams: over the air, the signature is collected and checked (secp256r1) when
  `--m17-signature-public-key <hex>` is provided (M17 2.0.11 signatures). The digest runs over the whole stream from its
  first frame: the LSF repeated in the LICH does not restart it. The signature is checked at the end of the
  transmission, and only when every voice frame arrived once and in order and every signature piece arrived; otherwise
  it is reported as incomplete (`SIG INCOMPLETE`), also when only the end-of-transmission marker ends it, never as a
  failed signature. A voice frame lost just before the signature frames cannot be detected, and then the check fails.
  M17 signatures are not verified on network (UDP/IP) streams: their signature frames are not played, and the stream
  shows `SIG NOT VERIFIED OVER NETWORK`. The SHA-256 stream hash and Authentication Frame of the M17 2.1 revisions are
  not implemented.
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

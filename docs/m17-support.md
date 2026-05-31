# M17 Support Scope

DSD-neo tracks the current published M17 air-interface specification for frame layout, sync words, FEC stages, LSF
semantics, stream frames, packet frames, BERT frames, and EOT handling. The M17 UDP/IP input and output support is a
DSD-neo transport for M17 frames; it is separate from RF air-interface channel-access behavior.

Implemented decode coverage:

- LSF, stream, packet, BERT, and EOT sync handling.
- Current LSF TYPE field semantics, including stream/packet mode, data type, encryption type/subtype, CAN, and signed
  stream advertisement.
- M17 address classification for reserved, standard, extended, and destination-only broadcast addresses.
- Stream metadata for text, GNSS position, and extended callsign blocks.
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

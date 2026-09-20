<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# Qt and Android decryption profiles

Choose **Decryption keys** in a saved system or scan entry. Profiles have stable
references and can be shared across sites. A profile selects one of these modes:

| Mode | Effect |
| --- | --- |
| Automatic keys | A managed collection or attached decimal/hex key CSV. Received identifiers select material where supported. An empty collection supplies no keys. |
| Direct override | Explicit channel/session material; disables automatic key loading. |
| No keys | An explicit empty set, distinct from inheriting another scope. |
| Advanced vendor mode | Standalone DMR keystream processing; stop and restart to change it. Scan and live replacement are unavailable. |

P25 supports received-ID collections including DES/RC4, AES and Phase 1 TDEA.
DMR additionally supports Basic Privacy, legacy destination lookup and up to 256
group-to-key-ID overrides. An unavailable or incompatible mapped key falls back
to the received ID; private radio IDs never use group mappings. NXDN scrambler,
DES and AES selection follow their respective decoder paths, including supported
destination/default fallbacks. dPMR uses a direct scrambler. M17 offers stream
scrambler seeds of 8/16/24 bits and AES of 128/192/256 bits. D-STAR and YSF do not
offer user-key decryption profiles. Signature verification is a separate feature.

Managed key IDs are hexadecimal; legacy destination IDs are decimal. Material is
masked, and an empty replacement field retains an existing value. Changing its
format requires replacement material. The library validates occupied keyring
indices, including AES segments and legacy destination hashing, and rejects
collisions. It does not turn the decoder's flat keyring into an algorithm-namespaced
database. Use separate profiles for incompatible collections. Attached legacy CSVs
retain their existing importer semantics; edit those files or build a replacement
managed collection explicitly.

Advanced DMR modes include TYT Basic/PC4/Enhanced, Retevis RC2, Baofeng PC5,
Connect Systems EE72, Kenwood, Anytone, static keystream and Vertex CSV. The editor
states the input width and scope; TYT Basic requires simplex reception. These
helpers do not all use the standard keyring or talkgroup map. Force privacy and
missing-algorithm fallback are separate from choosing key material. Inherit and
Use received identifiers have different meanings in a scoped profile.

Monitor decryption details show each observed slot's algorithm, signaled/effective
ID, key source, fallback and availability. **Key material available** does not
claim verified successful decryption. Last-observed information remains labeled as
such. Changing material invalidates old availability until the resolver reevaluates
it. Talkgroup policy remains separate: supplying a key does not unblock an excluded
talkgroup. Talkgroup details link to a matching collection entry or supported DMR
override; a P25 group action never silently replaces received signaling.

Live changes stay Pending until the decoder returns a retained result. Defaults
update the baseline beneath explicit scan profiles. An active-target change is
checked against target ID, tuning generation and key epoch, survives that target's
next visit, and leaves other targets intact. Preparation failures and stale requests
leave the prior configuration in place. Vendor processing requires a restart.
Applying an unchanged direct draft submits no command. Saving a profile changes
future starts; apply it explicitly to update a running session. A changed saved
revision is identified separately from the revision currently loaded.

Profiles are stored **unencrypted** in app-private `decryption_profiles.json` with
private generated CSVs under `decryption/`. Android backup remains disabled.
Existing direct keys migrate from `saved_systems.json` only after their profile is
saved successfully. Public model rows, snapshots, history and diagnostics contain
metadata and opaque references, never material. Private configuration and generated
scan CSVs must not be exported. QString/QML copies cannot guarantee memory erasure.
Removing a profile requires removing its saved and active-session references first;
attached profile files must be reassigned before removal from the file library.

Imported files → Channel decryption profiles shows each channel’s configured key
source, protocol, identifier policy and DMR mapping scope, without exposing key
material. Inherited rows use the defaults of the session starting the map.

Channel-map imports resolve companion key/group/map files into app storage and
validate the complete bundle before publication. Failed updates retain the old
bundle. See [CSV formats](csv-formats.md) and [scan scope](trunk-scan.md).

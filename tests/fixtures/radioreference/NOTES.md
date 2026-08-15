# RadioReference SOAP fixtures — capture notes

Captured 2026-08-15 against the live v18 API (`POST https://api.radioreference.com/soap2/`) with a
real premium account. Bodies are byte-exact `curl --output` captures with **two** edits, both
recorded here so nobody mistakes them for wire behaviour:

1. the `<username>` value in `user_data.xml` is the placeholder `user`;
2. one `<license>` value in `trs_sites_dmr_conv.xml` is the placeholder `CALLSIGN`. That field is
   an FCC/amateur licence callsign inside `siteLicenses`, and on this ham network it happened to
   equal the capturing account's RR username (RR usernames are often callsigns). Nothing parses
   `siteLicenses`, so the substitution has no effect on any test.

No request bodies are stored here, and no appKey, username or password appears in any file.

These files are the parsing contract for `src/runtime/radioreference/`. Everything below was
observed, not inferred.

## Fixture inventory

| File | Call | System |
|---|---|---|
| `zipcode_info.xml` | `getZipcodeInfo(52401)` | Cedar Rapids IA → `stid=19`, `ctid=841` |
| `state_info.xml` | `getStateInfo(19)` | Iowa; 102 counties, 1 statewide TRS |
| `county_info.xml` | `getCountyInfo(841)` | Linn County IA; 24 systems |
| `country_list.xml` | `getCountryList` | 236 countries |
| `country_info.xml` | `getCountryInfo(1)` | United States → state list |
| `trs_types.xml` / `trs_flavors.xml` / `trs_voices.xml` | `getTrsType/Flavor/Voice(0)` | 13 / 49 / 28 rows |
| `trs_details_p25.xml`, `trs_sites_p25.xml`, `trs_talkgroups_p25.xml`, `trs_talkgroup_cats_p25.xml` | sid 6673 | SARA Network, P25 Phase II — 35 sites, 1793 TGs, 64 categories |
| `trs_details_capplus.xml`, `trs_sites_capplus.xml`, `trs_talkgroups_capplus.xml` | sid 12574 | General Mills, DMR Capacity Plus Single Site — 1 site, 5 freqs |
| `trs_details_dmr_tier3.xml`, `trs_sites_dmr_tier3.xml`, `trs_talkgroups_dmr_tier3.xml` | sid 8697 | Alliant Energy, DMR Tier 3 Standard — 129 sites, `ch_id` populated |
| `trs_details_nxdn.xml`, `trs_sites_nxdn.xml`, `trs_talkgroups_nxdn.xml` | sid 12918 | Duane Arnold, NXDN NEXEDGE 4800 — 1 site, `ch_id` populated |
| `trs_details_edacs.xml`, `trs_sites_edacs.xml`, `trs_talkgroups_edacs.xml` | sid 220 | Hillsborough County FL, EDACS Networked Standard — 2 sites, 8 contiguous LCNs each |
| `trs_details_dmr_conv.xml`, `trs_sites_dmr_conv.xml`, `trs_talkgroups_dmr_conv.xml` | sid 9340 | Iowa DMR Users Group, DMR **Conventional Networked** — **36 single-frequency sites**, 14 talkgroups |
| `trs_sites_dmr_conv_small.xml` | sid 12244 | Linn County REC, DMR Conventional Networked — 2 sites, the ordinary small case |
| `user_data.xml` | `getUserData` | username scrubbed to `user`; `subExpireDate` `11-24-2026` |
| `fault_auth.xml` | `getUserData` with a wrong password | HTTP 500, `faultcode` `AUTH` |

`fault_subscription.xml` was **not** captured: no subscription-class fault was ever observed. An
expired premium account surfaces through `getUserData`'s `subExpireDate`, exactly as the plan
assumed. Nothing was synthesized.

## Corrections to the plan's API description

These are wire facts that contradict the plan text. Each is a small parameter/lexical detail, not a
shape change.

1. **`getTrsType` / `getTrsFlavor` / `getTrsVoice` must send `<id>0</id>`, not omit the part.**
   Omitting the element returns **HTTP 500 with a zero-byte body** (no fault, no XML at all).
   `id=0` returns every row (13 / 49 / 28); `id=1` returns just that row. The plan said to omit it.
2. **`getTrsTalkgroups` must send all four parts, with `tgCid=tgTag=tgDec=0` meaning "no filter".**
   Sending only `<sid>` + `<authInfo>` returns HTTP 500 with a zero-byte body. Sending
   `sid=6673, tgCid=0, tgTag=0, tgDec=0` returns the whole system (1793 talkgroups). The plan said
   the opposite — "omit the three filter elements entirely … zeros are untested and would plausibly
   filter to an empty set". Zeros are the correct and only working form.
   The same applies to partial omission: `sid` + `tgCid` alone is also HTTP 500/empty.
   Generalisation: this NuSOAP endpoint requires **every declared part of a message**; a missing
   part is a hard 500 with no body, which is precisely the "empty body → `DSD_RR_ERR_HTTP`" case.
3. **Response encoding varies by response path.** Successful responses declare
   `<?xml version="1.0" encoding="utf-8"?>`; **SOAP faults declare `encoding="ISO-8859-1"`**
   (`fault_auth.xml`). The plan claimed ISO-8859-1 throughout. This makes the
   `XML_ParserCreate(NULL)` rule *more* important, not less — the parser must honour whichever
   prolog arrives and must keep ignoring the HTTP `charset=utf-8` header.
4. **`siteModulation` is not one of `LSM`/`CQPSK`/`C4FM`.** Observed values across 168 sites:
   `CQPSK Phase 1` (1), `WCQPSK Phase 1 (NFM)` (3), `TDMA` (47), `C4FM` (1), `T3 DMR` (1),
   `DMR T3` (1), and `xsi:nil` (114). **No site anywhere carries the literal `LSM`.** A
   case-insensitive *equality* test for `"LSM"`/`"CQPSK"` — which is what the plan specifies for
   `dsd_rr_site_is_simulcast` — would never fire. Use a case-insensitive **substring** test for
   `CQPSK` or `LSM` (this also catches `WCQPSK`), keeping the `siteDescr` contains `Simulcast`
   rule, which does work: the SARA sites are named `Johnson Co Simulcast`, `Linn Co Simulcast`, …
5. **`tag` array members carry only `tagId`** — there is no `tagDescr` on the wire. Category names
   come from `getTrsTalkgroupCats`, as the plan already required.
6. **`TalkgroupCat` members observed**: `tgCid`, `sid`, `tgCname`, `tgSort`, `lat`, `lon`, `range`,
   `lastUpdated`. No `tgSortBy`.
7. **`trsSysidDef` members are individually optional.** P25 (6673) emits only `sysid` + `wacn`
   (`ct`/`model` absent entirely); Tier 3 (8697) emits `sysid`, `xsi:nil` `ct`, `xsi:nil` `wacn`,
   and `model=L`. The parser must tolerate both "member absent" and "member nil".
8. **`bandplan` and `fleetmap` are absent from every `getTrsDetails` response captured** — not
   empty arrays, simply not emitted. `bandplan_count == 0` for all five systems, so the
   "custom bandplan" preview warning is correctly inert here.
9. `stid` values are FIPS state codes (Iowa 19, Florida 12, Massachusetts 25, New York 36).
   Nothing depends on this; it is just a useful sanity check.

## Answers to the questions Stage 0 was required to settle

**Array member element name** — `item`, exactly as expected, e.g.
`<return xsi:type="SOAP-ENC:Array" SOAP-ENC:arrayType="tns:Country[236]"><item xsi:type="tns:Country">…`.
The `arrayType` attribute also carries a reliable element count, though the parser does not need it.

**`freq` lexical form** — `xsd:decimal` serialized as plain MHz decimal text. Across all 484
frequencies in the site fixtures:

- fraction digit counts: 1 → 8, 2 → 14, 3 → 22, 4 → 360, 5 → 79. **Maximum 5 fraction digits.**
- **No trailing zeros are ever emitted** (`852.2`, never `852.200000`; `851.05`, never `851.050`).
- **No whole-MHz value ever appears without a decimal point** — but the generator should still
  accept that form, since it is the natural serialization of an integer decimal and costs nothing.
- No sign characters, no exponent notation, no thousands separators.

So `dsd_rr_mhz_to_hz` needs: split on `.`, right-pad the fraction to 6 digits, reject 7+ significant
fraction digits. `851.0125` → `851012500`; `769.76875` → `769768750`.

**`use` values** — exactly the three the plan predicted: `d` (268), `a` (87), `xsi:nil` (128).
Nothing else. Note two shapes worth testing: EDACS sid 220 has **no `d` and no `a` at all** (every
frequency nil, so there is no control channel to seed), while the SARA Johnson County site marks
**every** frequency `d` — so "there is exactly one primary control channel" is not a safe assumption
in either direction.

**`faultcode` values encountered** — `AUTH` only. `faultstring` for it is
`Invalid Username or Password.  Make sure you are logging in with your username and not your email
address.` (note the double space, and that it is English prose — do not substring-match it).
`faultactor` and `detail` arrive as **empty strings**, not nil. Fault HTTP status: 500.

**What `lcn` contains for a P25 site — the load-bearing question.** It is a **small sequential
index starting at 1** (1, 2, 3, … per site), *not* a 16-bit P25 channel identifier
`(iden << 12) | chan`, and never above the site's frequency count. `ch_id` is `xsi:nil` on every
P25 frequency.

Consequence for Stage 6, per the plan's own P25 rules: the generator takes the "otherwise" branch —
emit a **sequential placeholder** in column 1 and attach the *channel-map half is unverified*
preview warning. The `-C` file is still valuable as the control-channel hunt list (column 2 in row
order), which is the reason P25 gets a map at all. The residual mistune risk is the bounded,
self-healing one the plan's risk register already describes.

**`ch_id` per protocol** — this is where it earns its keep:

| System | `lcn` | `ch_id` | `colorCode` |
|---|---|---|---|
| P25 6673 | 1..N sequential | nil | nil |
| Cap+ 12574 | 1..5 sequential | nil | present (e.g. `1`, `2`) |
| DMR Tier 3 8697 | 1..N sequential | **populated, 3-digit** (`302`, `347`, `308`, `292`, `406`, `281`, `295`, …) | nil |
| NXDN 12918 | 1, 2, 3 | **populated** (`1`, `2`, `3`) | nil |
| EDACS 220 | 1..8 contiguous | nil | nil |

The Tier 3 case is the proof that the `ch_id`-overrides-`lcn` rule matters: `lcn` is a meaningless
1-based row index while `ch_id` carries the real Tier III channel number (well inside the usable
1..4094 range). The NXDN case is the benign one where they agree.

**Leading-zero ZIP** — resolves correctly when sent as an int. `zipcode=2134` returned
`zipCode=2134`, `city=Allston`, `stid=25`, `ctid=1225`, i.e. 02134 Boston MA. So the UI may parse a
ZIP string to an int and send it without zero padding.

**US `coid`** — **1** (`countryName` `United States`, `countryCode` `US`).

**Version 18 confirmed served.** The v18-only fields are all present in the responses:
`enc` (talkgroups), `colorCode` and `ch_id` (site frequencies), `tdma_cc` (sites). `authInfo`
carried `version=18`, `style=rpc` on every call.

## Other observations worth keeping

- **`xsi:nil="true"` is pervasive.** Nil counts across the fixture set: `colorCode` 479, `ch_id`
  220, `tgSubfleet` 2120, `tgSlot` 2120, `siteCt` 168, `siteModulation` 114, `use` 128,
  `siteNeighbors` 82, `siteLocation` 81, `siteNotes` 66, `zoneNumber` 4, `zoneDescr` 4,
  `rectangles` 3, `ct` 2, `wacn` 2, `nac` 2, `aType` 11. The nil form is a self-closing typed
  element (`<use xsi:nil="true" xsi:type="xsd:string"/>`), so a character-accumulating leaf handler
  sees zero characters — exactly the trap the Stage-3 nil rule exists for.
- **`nac` is sometimes an empty string and sometimes nil.** DMR/NXDN/EDACS sites send
  `<nac xsi:type="xsd:string"></nac>`; two sites send it nil. Both must land as `""`.
- **Non-zoned systems** (nil `zoneNumber`/`zoneDescr`): Cap+ 12574, NXDN 12918, EDACS 220. The P25
  and Tier 3 systems are zoned. Both paths have coverage.
- **`tgSlot` is nil on all 2120 talkgroups captured.** It is still typed `xsd:string` in the schema;
  the struct keeps it as text.
- **`tgMode` values observed**: `D` (403), `T` (1487), `A` (230). **No `E`/`e` suffixed forms**
  (`DE`, `Te`) appear in this capture, though the plan reports them as real. Keep treating `tgMode`
  as an opaque string and keep `enc` as the authoritative encryption signal — `enc` is well
  populated: `0` (1756), `1` (16, partial), `2` (348, full). The 16 partial-enc talkgroups make the
  `partial_enc_as_de` toggle genuinely testable.
- **`TrsSite` member order on the wire** matches the WSDL order the plan lists exactly:
  `siteId, sid, siteNumber, siteDescr, zoneNumber, zoneDescr, rfss, nac, ran, siteNeighbors,
  siteLocation, siteCtid, siteCt, siteModulation, siteNotes, lat, lon, range, rectangles, splinter,
  rebanded, tdma_cc, siteLicenses[license], siteFreqs[lcn, freq, use, colorCode, ch_id], bandplan`.
  `TrsSiteFreq` is exactly the five members claimed.
- **`siteId` really is a database row id, not the RF site.** SARA sites 16863 / 23581 / 31563 carry
  `siteNumber` 1 / 1 / 10 — two different database rows share RF site number 1 in different zones.
  Anything user-facing must show `siteNumber` (plus `zoneNumber`), never `siteId`.
- **Non-ASCII coverage**: `trs_talkgroups_p25.xml` is the only fixture with high bytes — 10 of them,
  UTF-8 encoded, in the alpha tag `CR MERCY ER Ø` (`\xc3\x98`). That is the encoding regression
  case; it round-trips only if expat honours the `utf-8` prolog.
### Conventional Networked (DMR flavor 43, NXDN flavor 45)

These are catalogued as trunked systems but have **no trunking and no control channel**. The
Stage-6 classifier's `DMR … else → TIER3` fallback would otherwise classify them as Tier 3 and
generate a channel map for a system that has no control channel to find, so they get their own
protocol kind and their own generator path.

What the wire actually shows, across sid 9340 (36 sites) and sid 12244 (2 sites):

- **One site is one repeater, with exactly one frequency, and `lcn` is always `1`.** Every site in
  both systems has a single `TrsSiteFreq`. The LCN is therefore meaningless as a channel number;
  the site *is* the unit.
- **`colorCode` is populated per repeater** (`1`, `2`, …) and differs between sites. It is
  display-only for dsd-neo: there is no colour-code option in `dsd_opts`, because the DMR colour
  code is decoded off-air. Show it in the preview; never put it in a file.
- **`use` is nil on every frequency** — as it must be, there is no control channel.
- **`ch_id` is nil**, so the ch_id-overrides-lcn rule is inert here.
- **`zoneNumber`/`zoneDescr` are nil** — non-zoned, like Cap+ and NXDN.
- `siteNumber` is not necessarily small: the ham network uses DMR-ID-like values (310011, 311471),
  while the co-op uses 1, 2. Never assume a site number is an index.
- **`siteLicenses[license]` carries FCC/amateur callsigns.** Nothing parses it; see the scrub note
  at the top of this file.
- Site descriptions are plain place names (`Waukee`, `Ames`, `Marion`, `North Liberty`), which is
  what makes a multi-site picker usable.

**Why 36 sites matters:** `state->trunk_lcn_freq[]` holds 26 entries, so a scan list built from
this system must truncate and warn. That is the golden the fixture exists for.

**No NXDN Conventional Networked system was findable** in any of the 33 counties probed across
Iowa and Florida, though flavor 45 is in the live flavor table. The classifier branch is therefore
covered by the support-list fixture rather than by a system fixture.
- **NXDN flavors carry the 4800/9600 rate**, e.g. `NEXEDGE 9600`, `NEXEDGE 4800`, alongside
  `Icom IDAS Type C`/`Type D` and `Kenwood Type D`. The plan's classifier reads the *voice*
  description for `4800`/`9600`; the flavor is the more reliable source here, so check both.
- **EDACS flavors** present in the support list: `Standard` (6), `Standard w/ESK` (7),
  `Networked Standard` (8), `Networked Standard w/ESK` (9), `Narrowband` (10),
  `Narrowband Networked` (11), `SCAT` (22), `Extended Addressing` (39),
  `Extended Addressing w/ESK` (40). Note ESK **is** encoded in the flavor, contradicting the plan's
  "ESK is a user toggle, not a flavor" — it can be pre-set from the flavor and still overridden.
- **Type names** are exactly the plan's set: `Motorola`, `EDACS`, `LTR`, `MPT-1327`, `TETRA`,
  `Midland CMS`, `OpenSky`, `Project 25`, `iDEN`, `SmarTrunk`, `NXDN`, `DMR`, `Other`.
  Confirmed: the type name is `NXDN`; `NEXEDGE`/`iDAS` appear only as flavor descriptions.
- Linn County (`ctid=841`) alone contains P25, DMR Con+/Cap+/Tier 3, NXDN, LTR and Motorola Type II
  systems, so it is a good manual-test county — including the two blocked-type cases (LTR sid 2583,
  Motorola Type II sid 6744).

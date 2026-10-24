# Migrated text components

`transcode` uses ICU with per-call converter owners and no fallback transliteration
or Unicode normalization. UTF-8 and CP866/437/850/865, KOI8-R, Windows-1251/1252,
ISO-8859-1/15 have exhaustive representable-byte round-trip tests. Strict mode
rejects invalid sequences and unrepresentable output. Explicit replacement mode
uses U+FFFD while decoding invalid input and '?' for unrepresentable legacy output;
the result records whether replacement occurred so a caller can emit a safe diagnostic.

`parse_chs` retains classic level 1/2 and extended tables, one-way mappings,
deletion, up to four output bytes, escapes, comments and END markers. File and
line diagnostics identify invalid input. No inverse is inferred. Zero components
are omitted, as in the old byte-table format.

`load_charset_config` and `resolve_read`/`resolve_post` retain the legacy
first-match aliases and rule order. Group force beats global force; metadata
beats ordinary read defaults; missing metadata uses group then global defaults.
Read aliases match CHRS and optional CODEPAGE once. Post aliases match the MIME
label once; only forced group/global read defaults constrain explicit post-rule
destinations. `chsdir` is interpreted as before, relative to the process working
directory, not relative to the configuration file.

Existing explicit rules with neither a table nor `-builtin` preserve bytes,
including known source/destination labels and empty destination metadata. They
may relabel bytes and do not imply validated UTF-8. `-builtin` selects standard
conversion explicitly and cannot accompany a CHS file. Example:

```
read CP866 UTF-8 -builtin
post UTF-8 CP866 -builtin
```

When no explicit rule matches, known standard pairs use the codec: read defaults
to UTF-8 (or a requested concrete destination); post uses the forced destination,
then ordinary group/global default, then the source encoding. Unknown or wildcard
fallback destinations fail. An opposite custom table blocks automatic inference
of a reverse mapping, including equivalent standard charset spellings. A separately
configured reverse rule remains authoritative. Conversion plans own their mappings
and can outlive the parsed configuration. `-keepsoftcr` is retained in the plan for
the formatting stage. Configuration errors identify file/line, missing tables fail
closed, and bounded configuration input rejects embedded NUL.

MIME helpers decode base64, quoted-printable and RFC encoded words into validated
UTF-8. Encoded headers are folded on source code-point boundaries, with independently
valid encoded words. Binary control injection in headers and NUL in text bodies
are rejected. Text/plain 7bit/8bit/base64/quoted-printable bodies are supported;
HTML, multipart, malformed encodings and duplicate headers fail before storage.
Tests cover known vectors, full-byte base64 payloads, multibyte folding, adjacent
encoded words, corrupt input and deterministic mutated parser seeds.

The production NNTP executable uses these components for reads, posts and replies;
independent stored-byte and MIME e2e checks verify their service integration.

The DOS profile follows the project's shipped Unicode CHS files and the
[Microsoft CP437 mapping](https://unicode.org/Public/MAPPINGS/VENDORS/MICSFT/PC/CP437.TXT).
ICU's IBM converter convention is adapted explicitly for SUB/FS/DEL and the
CP437/865 MICRO SIGN mapping; this is a code-page definition, not Unicode
normalization. Greek U+03BC is rejected in strict CP437/865 output rather than
silently conflated with U+00B5. An independent Python codec reference fixture
checks every defined byte-to-code-point mapping, beyond round-trip-only tests.

Undefined Windows-1251/1252 bytes are rejected by strict conversion even where
an ICU alias supplies a C1 control mapping. Explicit replacement decodes them to
U+FFFD, and undefined C1 output is rejected or replaced by '?' under the selected
policy. Defined-byte reference tests and undefined-byte tests cover both cases.


`format_post_text` handles CR/LF input, Unicode NBSP at any position, historical
flowed joins, quote-space deletion and smart quote initials without fixed buffers.
`format_read_text` wraps on whole UTF-8 characters, preserves the signature line,
space-stuffs flowed text, dot-stuffs after conversion and emits CRLF. Hard limits
are checked in final encoded bytes (997-byte payload leaves room for a dot and
CRLF within the NNTP 1000-byte line limit). `encode_post_text` rejects oversized
encoded text rather than truncating; `append_tearline` retains the addcr/notearline
options and rejects multiline metadata.

Soft-CR filtering occurs in source bytes before conversion. UTF-8 continuation
bytes and DOS glyph bytes, including CP866 0x8D, are retained even without the old
workaround flag. In other legacy encodings, byte 0x8D is removed unless keepsoftcr
is explicit. The Unicode formatting layer requires valid UTF-8; byte-pass-through
plans remain distinct and cannot be passed off as validated Unicode.

## Production integration (2026-10-24)

The server now applies the documented resolver to actual JAM reads and NNTP POST,
including reply metadata and formatting options. Executable e2e tests cover all
ten supported standard encodings, special characters and pseudographics,
custom KOI8/CP866 mappings, explicit label-only byte pass-through, invalid UTF-8,
unrepresentable strict failures without mutation and replacement mode. Independent
Python decoders verify stored text and CHRS metadata rather than trusting the
production serializer. Final acceptance remains recorded in verification.md.

Textual X-JAM diagnostic headers (including PID and NOTE) pass through the same
selected read conversion as the ordinary text headers. The read model owns their
converted values; MIME encoding uses the transmitted codec. `IBMPC` plus `CODEPAGE`
is emitted as an unambiguous MIME name such as `CP866`. Original CHRS/CODEPAGE
records remain stored unchanged; decoded diagnostic values do not rewrite JAM.

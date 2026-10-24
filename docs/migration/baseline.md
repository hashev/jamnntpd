# Compatibility baseline

Source revision: `a05db5daf83d8277dd08bc1c64c20c5781d3a968`. `tests/fixtures/source-baseline.json` records every original source/header/build file and its SHA-256. The legacy implementation has not been executed as a protocol oracle. These contracts are derived from `ReadMe.txt`, `README.md`, `examples/*`, and the corresponding command handlers.

## Boundaries

Preserve existing JAM bytes, article numbering and `<number$group@JamNNTPd>` message IDs, per-session configuration reload, case-insensitive usernames/access letters, case-sensitive passwords, guest allow-list defaults and authenticated group replacement. `*` terminates legacy wildcard matching (the rest of the pattern is ignored); `?` matches one character. Netmail contents and overview entries require a matching sender or recipient realname. Cancellation additionally requires matching sender, local, unsent and unlocked attributes. A group prefixed `!` is netmail, `$` is local; `-nochrs` suppresses posted CHRS metadata. These prefixes are not part of the NNTP group name.

Existing flags and aliases are recorded in `tests/fixtures/protocol/cli.json`. A config option can omit its leading dash; `--option` is equivalent to `-option`; nested `config` and `create` are CLI-only. File paths and quoted names retain spaces. Port defaults to 5000, maximum sessions to 5. An explicit maximum of zero denies all sessions; an omitted origin override differs from an explicit empty origin override. ReadMe's documented POST bound is 20,000 bytes. CLI parsing truncation, unchecked numeric conversions, memory errors, leaked credentials and wrong successful I/O responses are defects, not compatibility requirements.

## NNTP

`tests/fixtures/protocol/session.json` holds exact CRLF-framed requests and responses for stable exchanges. `contracts.json` identifies every recognized command and its stateful scenarios. The welcome server name/version and timestamps are variable fields, not literal byte fixtures. XOVER preserves its empty byte/line columns. MIME supports text/plain, 7bit/8bit/base64/quoted-printable, rejects multipart/HTML and crossposts. Dot stuffing applies only at protocol boundaries.

Unit fixture validation checks framing and inventories only; it is not a server e2e pass. Full protocol playback and stateful article scenarios remain implementation tasks.

## Charset resolution

Read selection in `findreadxlat`: group forced encoding, otherwise global forced encoding; when absent use nonforced group default, then global default; then apply the first matching read alias once; select the first read rule matching CHRS/CODEPAGE and any destination constraint. An alias without a comma clears CODEPAGE. A pattern without a comma ignores CODEPAGE.

Post selection in `findpostxlat`: absent input charset uses defaultpost; apply the first matching post alias once; select the first rule satisfying source and destination patterns. `command_post` constrains destination to forced group encoding, otherwise forced global read encoding, otherwise unconstrained. Group default without `!` does not force posting. Explicit .chs mappings are directional and need not be invertible; their declared behavior wins over default codecs.

CHS noncomment header lines: ID, version, level, source, destination. ID >65535 maps all 256 bytes. Otherwise level 1 maps bytes 0..127 and level 2 maps 128..255. Unmapped bytes are identity. An optional trailing `END` marker, present in shipped tables, is accepted. Each row contributes up to four bytes; zero components are omitted (including deletion when all are zero). Supported escapes: `\\`, `\0`, `\dN`, `\xHH`. Row comments start with `;`; full comment lines are skipped. `-keepsoftcr` prevents removal of source byte 0x8D. Malformed numeric escapes must now fail rather than silently change data. No inverse is inferred for deletion/expansion.

## Independent binary fixtures

`tests/fixtures/jam` contains fixed little-endian records with deliberately high-bit values and full four-file sample base. Layout derives from JAM's documented fixed offsets (`src/jamlib/structrw.h`), not native C structure layout. The independent Python decoder uses explicit offsets and `int.from_bytes`; it does not invoke production serializers. CRC fixture values use ASCII case folding and standard CRC-32 complemented at the end to match JAM (no final complement). Tests compare known hex and intentionally corrupt lengths as well as decoding successful records.

Run baseline checks: `python3 -m unittest discover -s tests/baseline -v`. No external dependencies or real messagebases are used.

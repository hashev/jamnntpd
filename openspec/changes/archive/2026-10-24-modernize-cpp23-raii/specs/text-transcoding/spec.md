## Purpose

Обеспечить корректное чтение и отправку Unicode-текста в существующие FTN-базы с сохранением представимых символов, псевдографики и пользовательских таблиц преобразования.

## ADDED Requirements

### Requirement: Bidirectional standard encodings
The service SHALL convert between UTF-8 and CP866, CP437, CP850, CP865, KOI8-R, Windows-1251, Windows-1252, ISO-8859-1 and ISO-8859-15 for message bodies and textual headers, honoring group defaults, forced encodings, CHRS, CODEPAGE and configured aliases.

#### Scenario: Representable round trip
- **WHEN** representable Cyrillic, Latin, special symbols and box-drawing characters are read and posted through UTF-8
- **THEN** Unicode code points and the original legacy bytes round-trip without normalization, transliteration or accidental replacement

#### Scenario: Encoding selection
- **WHEN** CHRS/CODEPAGE, aliases, group overrides and defaults specify competing encodings
- **THEN** documented legacy precedence is applied and emitted metadata describes the actual stored and transmitted bytes

### Requirement: Explicit conversion failures
The service SHALL reject malformed input and unrepresentable output by default before modifying a base. A separately enabled replacement policy SHALL use a documented replacement, report the loss and never silently transliterate text.

#### Scenario: Unrepresentable Unicode
- **WHEN** a UTF-8 post contains an emoji absent from the destination encoding
- **THEN** strict mode rejects the post without storage changes and replacement mode stores the documented replacement with a diagnostic

#### Scenario: Malformed sequence
- **WHEN** input contains truncated UTF-8, overlong sequences, surrogates or invalid encoded text
- **THEN** strict conversion fails with a useful error without publishing a message

### Requirement: Custom CHS compatibility
The service SHALL retain custom .chs syntax and explicitly configured read/post directions, aliases, chsdir and keepsoftcr behavior. It SHALL NOT infer an inverse for a lossy or ambiguous table, and SHALL reject malformed tables with file and line diagnostics.

#### Scenario: User table
- **WHEN** configured tables contain deletion, expansion, escapes or asymmetric read/post mappings
- **THEN** conversions preserve those explicit byte mappings and use only a configured direction

#### Scenario: Unsupported reverse conversion
- **WHEN** only a forward custom table exists for a requested reverse conversion
- **THEN** the operation reports the missing conversion instead of inventing a mapping

### Requirement: Explicit rule semantics
The service SHALL preserve byte-pass-through semantics for existing explicit read/post rules without a CHS file. The new `-builtin` option SHALL opt a rule into standard transcoding and SHALL NOT be combined with a CHS file. A matching explicit rule SHALL take priority over automatic conversion; automatic conversion for known pairs SHALL remain available when no explicit rule matches.

#### Scenario: Existing label-only rule
- **WHEN** a matching explicit rule has neither a CHS file nor `-builtin`
- **THEN** the bytes remain unchanged, including when both charset labels are known

#### Scenario: Explicit built-in conversion
- **WHEN** a matching rule uses `-builtin`
- **THEN** conversion follows the resolved source and destination encodings and the configured strict/replacement policy

### Requirement: Unicode-safe MIME and formatting
The service SHALL process MIME headers, transfer encodings, folding, quoted text, NBSP options and soft-CR rules without splitting UTF-8 code points or confusing legacy glyph bytes with control markers.

#### Scenario: Header and body boundaries
- **WHEN** multibyte characters occur at folding, line-length or buffer boundaries in encoded headers and bodies
- **THEN** decoded text retains its code points, framing remains valid and encoded words are independently valid

#### Scenario: Legacy soft CR and NBSP
- **WHEN** CP866 byte 0x8D or UTF-8 U+00A0 is processed with applicable keepsoftcr/nonbsp settings
- **THEN** explicitly requested transformations apply without deleting a CP866 letter or damaging neighboring code points

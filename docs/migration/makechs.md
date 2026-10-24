# Migrated makechs

The C++23 target retains the original interface:

```
makechs <fromchrs> <destchrs> <frommap> [<destmap>]
```

Without destmap it emits a UTF-8 table; specify UTF-8 as destchrs. With destmap it
emits the historical directional legacy mapping. Unicode map entries use `0xBYTE
0xCODEPOINT`, optional # comments, identity defaults and the original '?' mapping
for explicitly unmapped nonzero source bytes. Supplementary Unicode scalars are
now supported; invalid scalars, escapes, out-of-range bytes and unreadable files
fail with a nonzero exit status and no partial stdout table.

The original 863 explicit fallback mappings from Johan Billing's version of
Markus Kuhn's transtab are retained. Using a fallback produces a warning while
generating the table. This historical table-generation operation is separate
from the strict/replace runtime codec policy: runtime conversion does not silently
transliterate. Existing user CHS files are not rewritten or inverted.

Generated classic/extended CHS tables retain their row ranges, zero-component
omission, escapes and END marker. Identical source/destination maps have a defined
classic result, correcting the original uninitialized firstdiff case. Explicit
ASCII remaps in destination Unicode maps are respected instead of being bypassed. Output is
buffered until generation succeeds. Tests cover UTF-8 supplementary characters,
identity maps, explicit fallback warnings, invalid input and the executable CLI.

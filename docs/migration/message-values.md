# Dates, addresses and command-line values

JAM locally created timestamps encode local wall time. The original
[JAM-001 specification](https://github.com/Mithgol/node-fidonet-jam/blob/master/JAM.txt)
defines that convention and the optional signed HHMM timezone subfield.
`local_jam_time` uses caller-owned `tm` through `localtime_r`/`localtime_s` and
computes the offset for the requested instant, including DST. It rejects dates
outside the fixed-width storage range before native conversions. No global
`gmtime`/`localtime` buffers or `mktime` guesses are used.

`format_jam_date` renders stored calendar fields and the recorded offset without
applying the host timezone a second time. An absent offset is rendered as GMT
without guessing the originating timezone. Invalid offsets fail; negative
sub-hour offsets retain their sign. Tests include a fixed leap-day date, the
uint32 upper boundary, eight concurrent callers and an isolated process with
`TZ=EST5EDT` for winter/summer and previous-day rollover.

FTN address parsing consumes complete unsigned components, retains legacy
optional-domain removal and zero-point normalization, and rejects overflow,
trailing junk and framing controls. Mailbox and origin parsing use owned strings;
quoted Unicode display names and escapes are preserved without fixed buffers.

`parse_command_line` retains option aliases, ordered `-config` application,
default-file lookup only when no options are supplied, sole-argument help aliases
and the `-create` action. Parsing does not write files; the executable performs
the returned action. `-config`/`-create` inside configuration files are rejected.
Configuration input has a bounded line and total size, rejects NUL, and rejects
non-regular files. Server executable integration remains outstanding.

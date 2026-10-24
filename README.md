# JamNNTPd 2.0 C++23 migration

This modified version replaces the original C implementation with a C++23 server
and `makechs`. It preserves the JAM on-disk format and traditional configuration,
NNTP commands, custom `.chs` tables and external tosser first-byte lock protocol.
Original authors and license notices are retained below, in ReadMe.txt and
src/jamlib/NOTICE; the jamlib LGPL text remains in src/jamlib/LICENSE.

Build with CMake >=3.28 and a C++23 standard library supporting `std::expected`.
Dependencies and commands: [build guide](docs/migration/build.md).
CMake builds `jamnntpd` and `makechs`; legacy Makefiles and OS/2 support are removed.

Run `jamnntpd -create jamnntpd.config`, edit paths and permissions, then
`jamnntpd -config jamnntpd.config`. Run in the foreground under a service manager.
Stop gracefully using SIGINT/SIGTERM or the Windows console control notification.
The service joins workers and flushes logs before exiting.

Logging uses spdlog. Standard text conversion uses ICU with strict errors by
default; `conversion_errors replace` explicitly permits replacement with diagnostics.
Read/post rules without a table preserve bytes. Add `-builtin` to opt into standard
conversion, e.g. `read CP866 UTF-8 -builtin` and `post UTF-8 CP866 -builtin`.
Custom tables take their explicit precedence; see [text conversion](docs/migration/text.md).

Back up a consistent `.jhr`, `.jdt`, `.jdx`, `.jlr` set while all writers are stopped.
For rollback stop all writers, restore the complete set and restart. Persistent
I/O failure quarantines the base when rollback cannot complete. No recovery journal
or power-loss transaction guarantee is provided. See [storage](docs/migration/storage.md).

Migration acceptance status: [verification](docs/migration/verification.md).
The presence of the new server does not establish that unfinished quality gates passed.

## Historical project documentation and changelog

# JamNNTPd (updated version)

JamNNTPd is an NNTP server that allows newsreaders to access a JAM messagebase. It can be used to read and write messages in Fidonet/FTN echomail and netmail.

Originally created by Johan Billing, it was later maintained by Robert James Clay, with contributions by Peter Krefting and others.

This fork includes several fixes and improvements (while trying to maintain backwards compatibility).

Carlos Navarro (@cnb), Fidonet 2:341/234 (ex 2:346/3)

Check the original documentation for JamNNTPd 1.3: [ReadMe.txt](ReadMe.txt)

## Changelog

Version 1.4-c (beta)

- Fixed incorrect response code when accessing a netmail not to/from the user

- Fixed crash when replying to a deleted or non-existent article

- The `-smartquote` setting (or its absence) now determines the default for reformatting quoted text to Fidonet style.
  Users can override this default with the new parameter `squote` (`squote=on` or `squote=off`)
  
- Fixed crash when accessing a deleted or non-existent article

- Fixed build error using GCC 15 - 'bool' cannot be defined via 'typedef' (thanks @axxisd)

- New `-note` config setting.
  Insert NOTE kludge with X-Newsreader/User-Agent string

- New `-def_addcr` setting (user parameter `addcr`).
  Default setting for adding a CR (empty line) at end of message text

- New `-loggroups` setting.
  Write groups selected by users to the logfile

- Fixed: "Re:" not removed from empty subjects

- New setting `-def_nonbsp` (user parameter `nonbsp`).
  Default setting for replacing non-breaking spaces by regular spaces (UTF-8 and iso-8859-1/LATIN-1)

- New setting `-def_delssq` (user parameter `delssq`)
  Default setting for deleting stuffed space stuffing quoted text (only in format=flowed)

Version 1.3.5-c 
- prevent issues when there is no FTN origin address, e.g. local areas. Thunderbird may show incorrect From's in headers. (SmapiNNTPd had already fixed this.)

Version 1.3.4-c
- fix issues with lines starting with a dot

Version 1.3.3-c
- fix text and header corruption issues

Version 1.3.2-c
- MODE READER patch: some newsreaders don't like response 500

Version 1.3.1-c
- fix level for CHRS: UTF-8
- use server version for PID kludge


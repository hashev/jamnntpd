# NNTP session component

`Session` owns group selection, current article, identity, pending login name and
user options. It receives callbacks for storage/content operations and does not
own sockets or call FTN serializers. The network loop and actual JAM/content
backend remain to be connected; these component tests are not server e2e.

Dispatch covers ARTICLE, HEAD, BODY, STAT, GROUP, LIST, NEXT, LAST, XOVER,
AUTHINFO, POST, HELP, QUIT and the original IHAVE/NEWGROUPS/NEWNEWS/SLAVE/MODE
responses. HELP matches the independent fixture. Numeric arguments and message
identifiers consume complete values and reject overflow. Empty/deleted/private
article access uses the backend's visibility result, including STAT. NEXT/LAST
skip absent or inaccessible numbers supplied by the backend. XOVER errors do not
publish a partial successful response on an I/O/corruption failure.

The backend must supply privacy-filtered visible numbers and must authorize every
article and submission using the identity provided on that call. Counts/bounds
are separate from visible numbers so strictnetmail compatibility can be retained.
The component checks group permissions itself before lookup. Successful login
replaces permissions and options atomically and clears a current group no longer
accessible. Failed authentication leaves established identity/options unchanged.
Options-only USER syntax remains supported. Password arguments are transient
views and are never copied into session state or error replies.

POST produces a receive action; only a successful backend submission produces
240. The forthcoming network loop must bound and unstuff the article, handle
incomplete input without calling submission, redact AUTHINFO before logging,
and retain logger lifetime beyond worker joins. Backend field conversion,
netmail checks and transactional mutation are still integration work.

## Production integration (2026-10-24)

The production `jamnntpd` target now connects `Socket`/`LineReader`, `Session` and
`JamBackend`. The backend applies fresh read/post permissions on every operation,
privacy filters strict netmail indices and individual article access, resolves
read/post conversion separately, and commits before returning success. Reply
ancestry and cancellation use native JAM records. Cancellation revalidates the
complete authorized snapshot while holding the storage lock.

The actual executable replays all characterization command transcripts, read/post/
reply/cancel and netmail ownership scenarios. The network loop bounds command and
POST lines at 1000 bytes, aggregate POST at 20,000 bytes, unstuffs once and closes
an oversized session. Incomplete POST never reaches submission. Socket operations
have a five-minute deadline and poll cancellation at most every 100 ms. Registry
and native JAM lock waits also observe stop; shutdown requests stop and joins all
workers before flushing the logger. The stress suite repeats contested shutdown
100 times, with an independent first-byte locker and unchanged blocked-post files.

The earlier descriptions of integration as forthcoming above record the component
stage; this section supersedes that status. Native platform acceptance and coverage
results are tracked in verification.md.

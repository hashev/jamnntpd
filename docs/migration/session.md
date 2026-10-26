# NNTP session component

`Session` owns group selection, current article, identity, pending login name and
user options. It receives callbacks for storage/content operations and does not
own sockets or call FTN serializers. The production network loop and JamBackend
connect through SessionServices; component and executable tests cover both layers.

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
240. The network loop bounds and unstuffs articles, refuses incomplete input,
redacts AUTHINFO and retains logger lifetime beyond worker joins. Backend conversion,
privacy and transactional mutations are checked through the real executable.

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

Native platform acceptance and coverage results are tracked in verification.md.

POST storage failures retain the historical `503` local-error status (I/O,
corrupt base or lock timeout). Invalid articles, encoding failures and permission
rejections retain `441`; posting without permission remains `440`. Diagnostics
from the backend are never copied into the wire response. Regression checks cover
all error categories, injected real-backend write/flush failures and a real TCP
POST with a missing companion file, followed by successful service recovery.

Storage lock timeouts during ARTICLE/HEAD/BODY/STAT are reported as local error
503, matching other unavailable-storage failures. They must not masquerade as
missing articles (423/430). XOVER likewise discards its accumulated response on
a storage timeout instead of returning a successful but incomplete overview.
Focused service tests cover numeric/id selection and a failure after one valid
overview record; a real TCP regression holds the native JAM byte lock past the
five-second storage timeout, verifies 503 and byte preservation, then verifies
the same article is accessible after the lock is released.

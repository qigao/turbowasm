# Native database capabilities for TurboWasm

Status: proposed, design review only. Tracking: #440. Baseline: TurboWasm v0.3.1
(`68df0b0b`), TurboDB v0.1.4 (`42f052a9`). No database adapter is implemented
or qualified by this document.

## 1. Goal and architecture

Untrusted application modules (including TurboP2P applications) can persist
local data, query relational services, and use Redis without raw host credentials,
file paths, driver modules, native pointers, or unrestricted sockets. The host
chooses capabilities; the guest never chooses native backends or connection URLs.

```text
WASM application -> guest SQL / Redis SDK -> core turbodb:db imports
                                      |-> future WIT component resources
  TurboWasm::Database (provider-neutral capability and owner)
                       -> native TurboDB adapter
                         |-> Orm::C + trusted SQLite driver (local file)
                         |-> Orm::C / MySQL driver (CNet + Crypto)
                         |-> TurboDB::Redis (TEDIS + NativeIO + CFlow)
                       -> existing host owner / scheduler / bounded worker lanes
```

Proposed implementation: a database capability module with public host header,
private resource/operation/policy code, guest C wrappers, and colocated formal
tests; a separate concrete adapter target composes the released TurboDB SDK.
The final package/target names are settled in #441/#442 after inspecting exports.
Runtime and Component remain database-neutral; no TurboDB transitive dependency
on TurboWasm::Runtime. The embedding host explicitly registers a capability.
Use existing resource/allocator/wait primitives, not duplicate OS, scheduler,
ORM, VFS or wire-protocol implementations. No feature-probe jobs or fallback
providers. Compose real modules in the native integration graph; use BUILD_TESTS
for formal tests.

Alternatives: SQLite compiled inside WASM needs a durable VFS (sync, truncate,
locking, recovery, WAL); existing WASI FS does not qualify these. Running database
protocol clients in WASM would duplicate driver/auth/TLS code and grant network
capabilities. Both remain possible separate projects, not this epic. PostgreSQL
may later reuse the SQL projection; Redis is not a relational SQL backend.

## 2. Verified upstream contracts

| Existing public contract | Design use / constraint |
| --- | --- |
| [TurboWasm link.h](https://github.com/qigao/turbowasm/blob/v0.3.1/wasm/include/link.h) | Host import registration, protected memory32/64 copies and resumable host waits; no guest pointer survives a native operation. |
| [execution.h](https://github.com/qigao/turbowasm/blob/v0.3.1/wasm/include/execution.h) | Owner-thread execution, pending wait identity, completion records status only, explicit resume; destruction interrupts retained callback. |
| [cflow.h](https://github.com/qigao/turbowasm/blob/v0.3.1/wasm/include/cflow.h) | Existing CFlow admission and deadline composition, not proof of a DB wait bridge. |
| [orm_runtime.h](https://github.com/qigao/turbodb/blob/v0.1.4/orm/include/orm/orm_runtime.h) | Explicit absolute driver module path, canonical manifest ID, bounded runtime, leased connections, checked close. |
| [orm.h](https://github.com/qigao/turbodb/blob/v0.1.4/orm/include/orm/orm.h) | Named-template raw query/bind; transactions; bounded materialized results; nontransactional typed async flows require a row shape and scheduler. |
| [SQLite async worker](https://github.com/qigao/turbodb/blob/v0.1.4/orm/backends/sqlite/async.c) | Serialized worker and interrupt; rejects active transaction/cursor. Cannot assume async transaction support. |
| [MySQL async source](https://github.com/qigao/turbodb/blob/v0.1.4/drivers/mysql/session_async.h) | CNet caller-driven progress; WAIT is not terminal, input borrow lasts until metadata or destroy; close must drain. |
| [MySQL transaction](https://github.com/qigao/turbodb/blob/v0.1.4/drivers/mysql/session_transaction.h) | Owner-affine transaction, destroy active result source before next command or terminal action. |
| [Redis pool](https://github.com/qigao/turbodb/blob/v0.1.4/drivers/redis/redis_pool.h) | Scheduler-affine fixed capacity, one lease per command stream, explicit waitable, cancel and destroy. |
| [Component Model](https://github.com/qigao/turbowasm/blob/v0.3.1/wasm/include/component.h) | Existing own/borrow and retained call contracts; generic DB host registration still needs verification; #404 tracks imported resource ownership. |

TurboDB v0.1.4 has matching release profiles for all seven current TurboWasm
platforms. Actual package exports, runtime libraries, plugin leases and upstream
dependency lock compatibility must be validated before integrating. MySQL links
CNet/Crypto; Redis links Core/CFlow and uses its NativeIO runtime. SQLite uses the
native engine and its own storage semantics.

## 3. Host policy, admission and quotas

Install one immutable capability configuration per instance before publication.
It is copied/retained by the capability owner, not read from guest memory after
admission. A shared host registry may own native drivers and pools, but instance
resource tables and policy are isolated. No global singleton.

Example (illustrative host configuration; not a released parser or ABI):

```json
{
  "aliases": {
    "local": {"driver": "sqlite", "host_file": "host-managed-app.db",
              "templates": ["item.get", "item.put"], "transactions": true},
    "orders": {"driver": "mysql", "credential_ref": "host:orders-read",
               "templates": ["order.by-id"], "transactions": false},
    "cache": {"driver": "redis", "credential_ref": "host:cache",
              "commands": ["get", "set", "del", "exists", "hget", "hset"]}
  },
  "limits": {
    "connections": 4, "transactions": 2, "cursors": 4, "operations": 8,
    "request_bytes": 65536, "result_bytes": 1048576, "total_host_bytes": 4194304,
    "columns": 32, "rows": 1024, "redis_arguments": 16,
    "transaction_deadline_ms": 10000, "operation_deadline_ms": 3000
  }
}
```

Values are illustrative defaults for review; every production policy has finite
positive limits for admitted resources and bounded aggregate work. Zero disables
a grant. Also cap queued work, parameters, each string/blob, Redis reply depth and
items, cursor idle time, connections per host, native DB work/time and file usage
where supported. Account before admission; keep charges until storage/lease is
actually released. Wasm instruction fuel does not bound native SQL CPU, disk or
network work. Apply driver/server role limits and statement deadlines, not
unrestricted native work. Do not claim hard cancellation for an uninterruptible
filesystem operation.

SQL policy: guest names a host-installed operation template, supplies typed
bound parameters, and optionally names its own transaction handle. Host owns
the SQL string, parameter/result schema, read/write class, permitted aliases,
transaction permission, and backend read-only identity. Do not concatenate
guest strings into SQL identifiers or use lexical SQL inspection as authorization.
Raw SQL, DDL, attach, arbitrary PRAGMA, dynamic plugin load and arbitrary DB paths
are not admitted in the initial scope. Trusted schema creation is host-only.

Redis policy: grant a small explicit command enumeration with exact argument
count/key positions, not an arbitrary command string. For single-key commands
construct keys using a host-assigned tenant/app/alias namespace with injective
length framing; field and value are counted bytes, not C strings. Deny AUTH,
SELECT, CONFIG, EVAL, KEYS, SCAN, arbitrary multi-key commands and scripts.
No credentials in logs, exceptions, guest payloads or config returned to guest.
Use host-side redacted audit IDs, backend status category and terminal outcome.

## 4. Proposed Core Wasm import contract

One shared import module `turbodb:db`; `db/sql.h`, `db/redis.h` and
`db/types.h` wrap these operations. These names/signatures are design targets,
not existing symbols. No version negotiation or parallel legacy ABI. Guest
memory index 0 in the first implementation; use i64 unsigned offsets (wasm32
wrappers widen) and checked lengths; qualify memory64 separately. Numeric
resource IDs are opaque unsigned i64, never addresses.

```text
submit(opcode:i32, owner:i64, request_addr:i64, request_len:i64)
  -> (status:i32, operation:i64)
poll(operation:i64) -> (status:i32, state:i32)
await(operation:i64) -> (status:i32, state:i32)
receipt(operation:i64) -> (status:i32, outcome:i32, byte_count:i64)
read(operation:i64, offset:i64, dst:i64, capacity:i64)
  -> (status:i32, copied:i64)
take_resource(operation:i64) -> (status:i32, resource:i64)
cancel(operation:i64) -> status:i32
drop(operation:i64) -> status:i32
```

Use multi-result scalars to avoid publishing handles through unchecked output
pointers. `submit` validates and copies the entire request, authorizes, reserves
operation/owner/result budget, and admits at most one native execution. On
rejection returns status and zero handle; never issues effects. Accepted work may
complete before await. `poll` never drives unbounded work. `await` checks
can_wait, parks through the existing host wait and returns state after resume;
no implicit execution restart. Non-resumable await returns UNSUPPORTED; it must
not re-submit. Host integration must guarantee admission can be serviced even
when a guest stops awaiting. `receipt` is terminal-only. `read` copies at most
capacity from immutable result storage after checking the entire actual range,
returns copied bytes and never changes a cursor or replays a command. A failed
copy leaves the result readable. Resource ownership transfer is scalar and
exactly once; an untaken result retains its resource. `cancel` requests
cancellation, not proof of rollback; `drop` only after terminal, and releases
untaken resources with checked drain. Explicit resource CLOSE is a submitted
operation; never block or free an active owner implicitly.

Operation families (opcode and status numeric assignments, exact structs and
golden byte fixtures finalized in #441 before public implementation):

| Operation | Owner | Request payload | Terminal result |
| --- | --- | --- | --- |
| OPEN_SQL / OPEN_REDIS | 0 | alias | connection/session handle |
| BEGIN | connection | requested isolation | transaction handle |
| EXECUTE | connection or transaction | template, typed parameters | affected rows |
| QUERY | connection or transaction | template, typed parameters | bounded cursor handle |
| FETCH | cursor | max rows | metadata/typed row batch, EOF |
| COMMIT / ROLLBACK | transaction | empty | terminal receipt |
| REDIS_COMMAND | Redis session | enumerated command, counted arguments | typed reply |
| CLOSE | resource | empty | terminal receipt |

Owner and opcode kinds are checked before provider invocation. One active
operation per connection/transaction/cursor unless provider explicitly supports
a different tested contract. A live cursor pins the query/connection/transaction.
In M2 materialize at most configured rows/bytes using ORM bounded result API;
cursor fetch slices the retained snapshot. FETCH failure after producing a batch
does not advance again; read the operation result or drop it explicitly. M3 may
add bounded incremental native cursors within the same guest contract.

Request/response encoding: little-endian fixed-width integers, length-prefixed
UTF-8 names/strings (validate UTF-8 and alias/template name grammar), arbitrary
counted blobs. Value sequence begins with u32 count followed by values encoded
as u8 kind + u32 payload byte length + payload. Proposed tags: null=0 (zero
bytes), bool=1 (one byte 0/1), i64=2, u64=3, f64=4 (eight bytes each), text=5,
blob=6; distinguish null and empty. Reject unknown tags, invalid fixed lengths,
overflow, excess counts, trailing bytes and unrepresentable native types.
SQL metadata is count + counted names/kinds; rows are count + value sequences.
Redis replies use bounded explicit nil/integer/blob/text/error/array kinds with
depth and item budgets, preserving binary values and nil/empty distinction;
unsupported reply types fail explicitly. No raw CMeta pointer, ABI layout,
C struct padding or provider pointer is serialized. #441 owns the complete
canonical layout and numeric table, and #442/#444 own backend-specific mappings.

API status (invalid handle, policy denied, limit, unsupported, pending, wrong
state, memory trap, etc.) is distinct from operation outcome. Pending states
include queued/running/waiting/cancel-requested; terminal outcomes distinguish
success, known failure/no effect, confirmed cancellation, and
OUTCOME_UNKNOWN (e.g. commit reply lost, mutation may have been applied).
Never report a write as failed-with-no-effect unless provider evidence supports
that statement; if effect uncertainty exists preserve it even on cancel.
No automatic retry. Connection failure can invalidate all dependent resources.
Use stable sanitized guest error categories, native detail host-side only.

## 5. Ownership, progress, and shutdown

Each host capability owner allocates non-reused resource/operation tokens within
its lifetime (checked counter, fail on exhaustion), records instance identity,
kind and parent lease, and checks the caller instance on every import. Lookup is
bounded by table limits. Same numeric values in unrelated capability domains
must never resolve to another instance's resource; identity validation is not
based on secrecy of a token. Completion identity includes owner + operation
generation, never a native pointer from guest memory. No resource publication
before admission succeeds. No reuse before drain, no callbacks while holding
table locks, no borrowed guest data in native I/O.

```text
FREE -> RESERVED -> ADMITTED -> RUNNING / WAITING -> TERMINAL -> DROPPED
                       |                |
                       +-- cancel requested -> cancel/drain -> terminal
```

Only the host owner mutates capability state. Copy input before native work;
worker completion returns owned bounded result and status to owner. If completion
arrives before wait registration, latch terminal state and consume it when
await starts; otherwise match the exact Runtime host-wait identity and use
execution_complete_host_wait, then resume on execution owner. Duplicate/late
completion cannot deliver into a retired operation. No native thread enters
Wasm or resumes its retained stack. Retained callback interruption requests
cancel; retain independent operation/lease until terminal drain. Do not free
provider storage just because guest execution was destroyed.

Use existing CFlow scheduler/executor and NativeIO progress. For SQLite use
a bounded serialized connection lane for synchronous connect/transaction/commands
and materialized queries; use the existing ORM async flow for appropriate
nontransactional typed queries only. MySQL nontransactional asynchronous typed
queries can reuse ORM flow with row shape; a native prepared async source is
an alternative if explicitly selected, not fallback. Its public WAIT contract
requires the caller to progress it; do not invent a NativeIO waitable not exposed
by the API. For synchronous transaction/control methods schedule on a bounded
owner lane without changing an active session's owner thread. If a required
upstream waitable/async control API is absent, track and qualify it in TurboDB
before relying on it. Redis pool operations and stream steps are scheduler-affine;
arm existing CFlow waitables and keep leases until terminal/destroy. No busy wait,
duplicate scheduler, implicit retries or raw blocking DB call on the Wasm owner.

Shutdown: stop admission; cancel/drain pending operations on their owners; release
retained results and cursors; roll back known-active transactions, resolve or
record uncertain terminal outcome; checked close/release connections and Redis
leases; release runtime/driver modules after dependents; retire capability.
On BUSY preserve owner/lease for retry by host teardown policy, do not discard
a live callback. Keep an explicit bounded quarantine for undrained operations;
do not publish capacity as free while native work is alive. Fail host shutdown
according to existing fail-fast contract if drain cannot be established.

## 6. Component projection and qualification

Core ABI first, with compiled real guest fixtures (not just C mock calls). Later
WIT uses application-owned SQL and Redis interfaces with own/borrow connection,
transaction, cursor and pending operation resources plus typed values; not a
standard WASI database interface. Verify public generic capability import
registration and per-exec nominal resource ownership (#404); do not make WIT a
second DB implementation. Metallic C11 guest SDK remains unqualified in v0.3.1;
use pinned compiled Wasm fixtures until separately verified.

| Stage | Issue | Acceptance gate |
| --- | --- | --- |
| M1 | #441 | deterministic provider + real guest; ABI, limits, policy, hostile memory, wait/cancel/drain |
| M2 | #442 | native SQLite file persistence, named queries, typed rows, transaction and isolation tests |
| M3 | #443 | MySQL parameterized requests, bounded cursors, transactions, real disposable server and lost reply |
| M4 | #444 | Redis allowlist and namespace, pool pressure, binary replies, real disposable server and cancellation |
| M5 | #445 | multi-instance lifecycle, mixed providers, faults and shutdown; sanitizers where supported |
| M6 | #446 | WIT projection, SDK package and full seven-profile gate |

Security and ownership are acceptance criteria in every slice, not deferred
until M5. Native desktops run formal CTest with real WASM and SQLite; real
MySQL/Redis service integration on Linux CI, with portable deterministic
transport/provider tests on other desktops. Run interpreter and supported MIR
variants; Windows ARM64 interpreter, Android x64/ARM64 cross-build/install only.
Linux x64/ARM64, Windows LLVM-MinGW x64/ARM64, macOS ARM64, Android x64/ARM64.
No Intel macOS, MSVC, Mono, installed-consumer or probe tests. Validate exact
release SDK locks/digests and driver module path/lease; do not ship native
credentials. Publish package manifest with source SHA, dependency family,
platform/compiler, actual CTest evidence and scope limitations. No performance
claim until measured; measure bounded row transfer, native vs guest overhead,
queue delay and cancellation drain. Benchmarks separate from BUILD_TESTS.

Rollback: keep the embedding host on the preceding qualified release and omit
the capability registration; guests importing it fail link/admission, not
silently use another database. Do not change Runtime ABI or native database
storage format for this adapter.

## 7. M1a private wire admission slice (#441)

The private implementation `wasm/src/database_wire.{h,c}` pins request opcode
and value numbers, checks canonical little-endian requests and performs
allocation-free admission against host policy. It is not installed; no
production `turbodb:db` import or provider operation exists yet.

| Opcode | Value | Exact payload (u32 and all lengths little-endian) |
| --- | ---: | --- |
| OPEN_SQL | 1 | counted name |
| OPEN_REDIS | 2 | counted name |
| BEGIN | 3 | u32 isolation: default=0, read-uncommitted=1, read-committed=2, repeatable-read=3, serializable=4 |
| EXECUTE | 4 | counted template name, u32 count, typed values |
| QUERY | 5 | same as EXECUTE |
| FETCH | 6 | u32 nonzero row count |
| COMMIT | 7 | empty |
| ROLLBACK | 8 | empty |
| REDIS_COMMAND | 9 | u32 command, u32 argument count, counted byte arguments |
| CLOSE | 10 | empty |

Counted bytes are u32 length then bytes. Names must match
`[a-z][a-z0-9_-]*(\.[a-z][a-z0-9_-]*)*`; nonempty, exact match, no NUL.
Value tags are null=0, bool=1, i64=2, u64=3, f64=4, text=5, blob=6;
fixed sizes are enforced, UTF-8 is strict, NUL in counted text is not a
terminator. Redis commands GET=1, SET=2, DEL=3, EXISTS=4, HGET=5, HSET=6
have exact arity 1,2,1,1,2,3 respectively; first argument is a nonempty
logical key. Host owns the namespace transformation (later M4).

Status: OK=0, INVALID_REQUEST=1, DENIED=2, INVALID_HANDLE=3, LIMIT=4,
UNSUPPORTED=5, PENDING=6, WRONG_STATE=7, BUSY=8, OUT_OF_MEMORY=9.
Outcome: SUCCESS=1, FAILED_NO_EFFECT=2, CANCELLED_NO_EFFECT=3,
OUTCOME_UNKNOWN=4 (terminal receipt implementation pending). The decoder
does not allocate, invoke a provider, or publish a partial request. OOB
guest-memory errors propagate Runtime trap status, never a fake database
success. Limits and authorization precede external effects. Each SQL template
has an explicit operation mask and parameter tag masks; no implicit casts.
The guest copy boundary compares the live opaque instance identity, because
the Runtime host call exposes a transient facade, not the original C handle.

Formal tests use a checked-in 129-byte freestanding guest compiled from
`wasm/tests/fixtures/database_admission.c`, actual import calls plus direct
golden byte tests. The import module `fixture:db` exists only in the test;
its resolved owner identities are fixture data, not a production handle table.
M1b must copy and validate policy ownership, reserve resource/operation
capacity, own staged bytes, implement generation-safe handles, actual submit,
terminal receipts, await/cancel/drain and resource transfer. Do not connect
native TurboDB providers or claim #441 complete before those tests pass.

## 8. M1b1 private lifecycle core (#441)

The manager preallocates resource and operation tables, copies policy names,
templates and parameter masks, and reserves fixed request/result bytes per
operation within one aggregate logical host-byte budget. All resource capacity
counts include reserved, held, live and quarantined resources. The owner thread
is the only mutator; there is no scheduler or provider invocation in this slice.

- Submit accepts a host-owned snapshot, validates it, and reserves its operation
  plus prospective resource before dispatch. Start is a one-time transition
  from ADMITTED to RUNNING and returns a private provider work view.
- IDs are monotonically allocated in the capability domain, not slot indexes;
  an old completion cannot target a reused slot. Cross-instance checks belong
  to the later import bridge, which must authenticate every operation.
- A transaction or cursor pins its parent. A connection with a live child or
  pending operation rejects conflicting work. Successful begin/query reserves
  a held resource that transfers exactly once.
- Cancellation before start is known no-effect. Cancellation after start only
  requests provider notification once; no quota or owner is released until
  terminal completion. An unknown outcome quarantines owner and ancestors;
  only explicit cleanup remains admissible. Successful commit/rollback/close
  consumes the owner only with provider drain proof.
- A terminal result is immutable and bounded. Reads are repeatable and never
  replay provider work. A terminal operation holding an untaken resource cannot
  be dropped until the host drains it and confirms disposal. The guest must
  never be able to invoke that confirmation directly.
- Stop closes business admission but allows cleanup CLOSE; destroy returns BUSY
  while any resource or operation remains. All hot-path allocations are made
  during initialization. Tests inject initialization allocation faults and
  assert that no later allocation is required.

The private lifecycle test exercises deterministic provider transition inputs,
including reservation exhaustion, copied request and policy, duplicate/stale
completion, transaction/cursor dependency, uncertain outcome, cleanup and
allocation failures. This does not yet qualify provider dispatch, native driver
cancellation, actual guest submit/await/read/drop imports or owner-affine
completion routing. Those are M1b2 and later native backend slices.

## 9. M1b2 private Core import bridge (#441)

`database_bridge.{h,c}` composes the existing admission and lifecycle core
with the eight proposed Core imports. The adapter is internal, not installed.
An owner initializes it with immutable copied policy and nonblocking
provider start/cancel callbacks. Registration precedes instantiation; the
owner explicitly binds the resulting live instance. Before binding (including
module start) and after stop, all imports deny access. Every import checks the
live opaque instance identity before reading memory, looking up handles or
dispatching. The embedding host must preserve the bridge, provider and instance
lifetime contract; a new instance requires a new bridge.

Submit copies protected guest memory into precharged scratch, then transfers
a snapshot into reserved manager storage. It dispatches exactly once. The
provider may complete inline or post a terminal receipt to the same owner.
No provider callback may block the Wasm owner, reenter guest code or retain
a guest pointer. The bridge charges its scratch and metadata against the same
host budget. Read copies immutable result data through the protected runtime
memory API; a trap cannot replay or consume a result.

Pending await is allowed only in an execution resumed through the bridge
owner entry point. The exact execution identity and Runtime generation-tagged
wait are retained until resume/unwind. Owner completion records a receipt;
wake verifies the matching execution/wait and records the Runtime completion
without running guest instructions. A completion before await returns
immediately. Destroying a suspended execution sends one cancellation
notification and unwinds its callback; native operation storage remains
owned until terminal drain. Stop denies guest work; after executions,
instances and linkers are destroyed, detach gives trusted host cleanup access
to the manager. Destroy remains BUSY until resources are drained. A complete
automatic shutdown enumerator, scheduler integration, native providers and
installed guest SDK are follow-up gates.

A Core Wasm fixture implements all eight exact multi-result signatures plus
memory read/write instructions. The deterministic provider test covers
unbound and wrong-instance denial, copied requests, missing imports,
completion before and after await, exact wake, repeat/invalid completions,
cancellation, guest output OOB followed by successful retry, quarantine
and explicit close, suspended execution destruction and late terminal drain.
Register interpreter and actual MIR variants in the normal formal CTest graph.
Native driver effects remain outside this slice.

## 10. M1b3 checked discard and owner-driven shutdown (#441)

Guest drop of a terminal operation with an untaken resource now relinquishes
the original result and starts a separate asynchronous native CLOSE in the
same preallocated operation slot. It never requires spare operation capacity.
The guest keeps its operation token; the provider receives a fresh dispatch
token. A late business completion cannot finish disposal. Poll/receipt now
refer to cleanup; repeat drop returns PENDING until a terminal receipt, then
OK consumes a successful cleanup. Failed/unknown cleanup returns WRONG_STATE
and retains its receipt and unpublished resource. Take and cancellation of
discard are forbidden; interruption may unwind await but cannot cancel cleanup.
Uncertain disposal quarantines ancestors without publishing the held resource.

The host stops admission, destroys any retained execution and guest instance
and linker, then detaches. One call to shutdown_step makes at most one
provider start/cancel action. CANCEL requests cancellation once; WAIT means
drive the existing provider/scheduler and deliver terminal completion on the
owner, not spin. After all admitted business drains, terminal receipts are
retired with their outcome (the host records uncertainty), held results are
disposed, and native resources close leaf-first. A transaction CLOSE must
roll back known-active work and drain its native identity; never retry an
unknown COMMIT. CLOSE failure preserves its cleanup identity and receipt.
BLOCKED stops automatic cleanup, including repeated steps; explicit trusted
host recovery may confirm an independently drained held resource or failed
shutdown close. It cannot forge success or retry business work. Checked
destroy remains BUSY until every operation and resource is retired.

No new scheduler, public header, driver API, or host-path allocation is
introduced. The existing Core WASM fixture tests one-slot discard, inline and
deferred disposal, stale business completion, forbidden take/cancel, unknown
disposal, shutdown with live connection/transaction/cursor and held result,
one-shot cancellation, child-before-parent close, sticky failed close, explicit
host drain proof and late completion. Interpreter/MIR tests use the same
formal CTest graph. Native providers must still supply actual cancellation,
rollback, deadline and drain guarantees; CFlow integration and the released
database SDK are later gates.

## 11. M1c1 native serial worker and owner completion mailbox (#441)

`database_worker.{h,c}` is the private dispatch adapter for synchronous,
thread-affine native database operations. It uses the released CMeta CFlow
serial Executor and its bounded typed event Mailbox; it introduces no custom
thread pool or scheduler. One dedicated lane per adapter retains native
connection/transaction/cursor affinity. This is the blocking-native backend
path (e.g. SQLite and synchronous transaction control), not a replacement
for the existing asynchronous MySQL/Redis progress APIs.

The worker has fixed operation slots and result reservations; its own
logical byte budget and CFlow fixed queue/mailbox capacities are charged
separately from the bridge/manager budget. Each slot owns one completion
mailbox position. Submit copies the work descriptor, not guest memory:
the manager pins its request snapshot through terminal receipt. Executor
admission failure means the provider was never entered and becomes
FAILED_NO_EFFECT, not a retry. The serial callback returns an outcome,
native resource and byte count into its reserved storage. Finalize publishes
the slot index through the CFlow mailbox and makes no further slot accesses.
The owner receives the synchronized record and completes the manager;
the worker never touches the bridge or guest execution during native work.
A malformed provider receipt stops progress and preserves the occupied slot
and native ownership instead of fabricating success. An impossible mailbox
publication failure is fail-fast.

A borrowed single-consumer CFlow waitable signals completion; the host may
arm its existing nonblocking wake and call progress on the owner, one record
per step. It separately calls bridge wake/resume for the exact execution.
Cancellation is an atomic cooperative signal. A queued call interrupted
before entering the provider is known no-effect; an entered call must
classify its own outcome, even after cancellation. The monotonic deadline
includes queue delay and is exposed to the provider. It is not a guarantee
that an uninterruptible native call has stopped. Worker storage survives
guest teardown until terminal drain. Destroy requires all slots retired,
waker disarmed, and joins the executor before freeing the mailbox or
provider callback state.

Formal real-WASM interpreter/MIR tests use a deterministic native callback
on an actual CFlow worker, with owner-thread completion, thread affinity,
bounded result copy, blocked running cancellation with unknown outcome,
queued cancellation before native dispatch, and ordered shutdown/drain.
Native SQLite durability and driver lifecycle are still M2 (#442);
nonblocking MySQL/Redis integration is separately tracked in #443/#444.

## 12. M2a native SQLite command slice (#442)

Pin TurboDB v0.1.4 (source 42f052a958ffac29e93b47cf63956fcb255762e3,
producer 45235859548) in the existing SDK lock and curl restoration path.
The producer's CI records the exact same cache 0.1.9, Base 0.1.11,
CMeta 0.1.11, Utils 0.1.10 and CNet 0.1.20 inputs; all seven target archives
and manifest are bound by their published sizes and SHA-256 digests.
The new private target links Orm::C; TurboWasm::Runtime does not link it.
The driver module is explicitly loaded from the host's absolute path, using
canonical driver ID sqlite. No new download mechanism or consumer probe.

This initial provider admits only OPEN_SQL, EXECUTE and CLOSE with host
immutable configuration: absolute pre-existing database file, read-only or
read-write mode, and one-to-one trusted single-statement DML templates
matching the copied policy ordinals. Paths and SQL templates are borrowed
host configuration through worker join; no guest path or SQL crosses the
boundary. Initialization rejects grants for Redis, queries or transactions,
rather than advertising incomplete operations. Host prepares its schema;
SQLite open does not create files implicitly. Read-only aliases admit no
write templates and use native open_mode=read_only. The serial worker owns
the ORM runtime and fixed native connection slots, loads the driver explicitly,
binds typed counted values through orm_raw/orm_query_bind, and closes/releases
leases and runtime on that same lane. Successful EXECUTE returns an unsigned
little-endian 64-bit affected-row count. Failure before entering query execution
is known no-effect; any failure after entry is conservatively unknown and
quarantines the owner. The synchronous ORM API cannot guarantee hard
interruption once entered; no fake cancellation or rollback claim.

Formal interpreter/MIR tests use the real Core Wasm imports, CFlow worker,
released SQLite plugin and a native file. Host-only schema setup precedes
guest work; the guest inserts a parameterized binary value including NUL,
reads the affected-row receipt, opens a read-only alias and observes policy
denial, then tests a duplicate-key failure and owner quarantine. Shutdown
closes the remaining native connection. A new host ORM connection after
worker join verifies the persisted row and exact blob; it is not an installed
consumer test or guest query. Query/FETCH result codec and transaction
commit/rollback are subsequent M2 slices; no native query-from-WASM or
transaction support claimed here. Android remains cross-build only.

### Windows explicit-path plugin loading (M2a CI #930)

CMeta v0.1.11 loads absolute plugin paths with
`LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS`.
The ordinary Windows PATH used to start the test executable is not sufficient
to admit dependencies of a separately installed plugin. The Windows embedding
host must explicitly register its trusted SDK DLL directories before loading
the driver. The native SQLite test registers the selected TurboDB, SaltsBase, SaltsUtils,
SaltsCNet and exact vcpkg target triplet bin directories with AddDllDirectory,
retains their cookies across setup, worker execution and final reopen, and
removes the directories only after all runtime/plugin leases are unloaded. No global
loader-policy change, implicit discovery, DLL copying or fallback loader.
Print the ORM error category/detail if driver admission fails; do not treat
an OS loader failure as a database query failure. This fix targets the
Windows-only driver-load failure observed before SQL execution in CI #930.

CI #931 confirmed ORM_STATUS_DRIVER_LOAD_ERROR (21, load driver Plugin:
load_failed) on both Windows architectures after the initial three SDK bin
directories were admitted. The producer CI 45235859548 deploys runtime DLLs
from the selected vcpkg triplet/bin and Salts SDKs before running formal
plugin tests. Both locked LLVM-MinGW triplets in vcpkg-cache v0.1.9 declare
VCPKG_LIBRARY_LINKAGE dynamic. The first fix omitted this runtime closure.
Extend the explicit test host registration to the matching vcpkg triplet/bin
and SaltsCNet/bin (CMeta's static SDK has no bin requirement). This does not
alter the loader, use PATH as a plugin search policy, copy DLLs or disable
tests. The exact missing PE import is not reported by the released loader;
the corrected closure still requires the actual Windows CTest result.

## 13. M2b native SQLite queries and bounded result cursors (#442)

The SQLite provider adds trusted named QUERY templates, FETCH and cursor
CLOSE. A read-only alias grants QUERY only and still opens the native database
with read_only mode. Host-installed QUERY SQL must be a single side-effect-free
SELECT (no mutating functions, RETURNING or other state-changing statements);
there is no guest SQL text and no string-based guest SQL authorization.
Transaction and Redis grants remain rejected. Connection and cursor slots
are separate, worker-owned and bounded. Each cursor retains one immutable
TurboDB ORM materialized result, with native row, column and payload-byte
limits. Native allocator overhead is separate from the preallocated slot
and result-transfer budgets.

The exact private result encoding is now:
- EXECUTE: u64 affected rows (unchanged).
- QUERY: u32 columns, u32 flags (bit 0 names present; all other bits zero),
  u64 total rows, followed when flag is set by u32 length and counted UTF-8
  column names in native order. In this ORM version an empty result provides
  a column count but no materialized names; return flag=0 without inventing
  labels. SQLite storage types are dynamic, so cell kinds are carried with
  each row value, not inferred from the column declaration.
- FETCH: u32 row count, u32 EOF (0/1), then rows of u32 column count followed
  by existing canonical tagged values (u8 kind, u32 payload length, bytes).
  All integer fields and binary64 bits are little-endian. Counted UTF-8
  text, BLOB and NULL remain distinct, including empty payloads.

Preflight every materialized row against the worker's precharged result
capacity and max-value limit before publishing a cursor. Reject an
unrepresentable/oversized result rather than returning an unusable handle.
FETCH encodes as many complete rows as fit up to the requested count; it
advances only after successful batch creation. Re-reading the terminal
operation (including after a guest OOB trap) does not fetch again. An empty
page at end is stable. The existing parent pins and same-slot discard
ensure native result storage is released before its connection closes.
The native ORM result is materialized, not an unbounded streaming SQL
cursor. Native execution errors remain conservative; a projection failure
after a successful trusted read-only SELECT is known no-effect.
 
The existing interpreter/MIR native tests now cover actual SELECT through a
read-only alias, seven dynamic typed columns (integer, binary, NULL, empty
BLOB, empty TEXT, float, UTF-8), exact metadata, byte-bounded pagination,
repeat EOF, output trap/re-read, parent/limit admission, stale cursor tokens,
empty result metadata, oversize row rejection, and real untaken cursor
disposal. Transaction execution is still M2c; the adapter is still private.

## 14. M2c worker-owned native SQLite transactions (#442)

The provider adds a bounded transaction slot table. The guest's resource
manager reserves a transaction and pins the connection before dispatch.
The worker reserves the native slot before BEGIN and retains the actual
ORM transaction and its parent connection index on the same CFlow serial
lane. Named EXECUTE and QUERY route through orm_query_execute_in_transaction;
the same canonical result cursor can be parented by a transaction. A live
cursor prevents terminal transaction control until it is closed.

SQLite v0.1.4 advertises ORM_DRIVER_CAP_TRANSACTION but no named isolation
flags. Its implementation issues ordinary BEGIN for every valid ORM
isolation enum. Admit only TW_DB_ISOLATION_DEFAULT on a writable alias;
ORM_ISOLATION_SERIALIZABLE is the API carrier for native BEGIN, not a
separate guest promise of a selectable isolation level. Reject explicit
isolation and read-only transaction grants at initialization, and deny
ungranted BEGIN before dispatch. Savepoints are not exposed.

Successful COMMIT or ROLLBACK consumes the transaction only after checked
orm_transaction_close and release. CLOSE of a known-active transaction
rolls it back; this includes automatic untaken-result disposal and
detached shutdown. A failed COMMIT returns OUTCOME_UNKNOWN conservatively,
retaining the transaction and quarantining its parent. Do not retry COMMIT.
If ORM specifically reports COMMIT_UNKNOWN, its checked close contract
allows release without another commit or rollback. Otherwise CLOSE
attempts rollback on the retained active transaction. Failed rollback or
checked close is sticky, preserves native ownership and requires explicit
host recovery or fail-fast shutdown; no legacy destroy/implicit retry.
The host's monotonic operation deadline and cooperative cancellation do
not guarantee interruption of an already entered synchronous SQLite call.
A hard transaction-duration watchdog is not supplied by this slice.

Extend real WASM/ORM interpreter and MIR CTests with default isolation,
read-only and unsupported isolation denial; in-transaction write/read and
cursor pin; visibility on a second native connection before/after commit;
explicit rollback; stale transaction rejection; untaken BEGIN rollback;
actual deferred foreign-key constraint failure at COMMIT, no replay,
checked rollback/close; and detached shutdown of an active transaction
with a live cursor. After all leases and workers drain, reopen the native
file and verify only the original and committed rows survive, with exact
binary values. No new CI profile or test project. The adapter remains
private; MySQL/Redis, hard interruption, full fault qualification and
installed guest/host SDK remain separate gates.

## 15. M3a common native ORM provider and real MySQL TLS qualification (#443)

Replace the private SQLite-only provider with database_orm.{h,c}; no legacy
wrapper or duplicate SQL execution engine. Select exactly one driver per
provider/instance: SQLite canonical ID sqlite or MySQL canonical ID
drivers/mysql. Both share typed request binding, bounded materialized
result projection, CFlow serial worker, resource slots, transaction and
checked cleanup logic. Both use the same released Orm::C / TurboDB v0.1.4
SDKs; MySQL's driver implements CNet/Crypto transport and binary prepared
parameters. Runtime remains independent of the database link interface.

MySQL host aliases carry host, port, user, password, database, absolute CA
path and expected server name. Require all fields except that an explicitly
empty password is allowed; the underlying driver always verifies TLS.
No raw guest connection settings, plaintext downgrade or driver fallback.
MySQL read-only aliases require a host-provisioned server account with
SELECT-only privileges and only QUERY templates. No unsupported claim that
client-side metadata verifies server privileges. SQLite retains its native
read-only mode. MySQL accepts explicitly granted READ_COMMITTED,
REPEATABLE_READ and SERIALIZABLE after checking driver capability bits;
SQLite continues to expose only its default isolation. MySQL's ORM
connection is a logical admitted endpoint, not a permanently connected
socket: ordinary commands and materialized reads open dedicated authenticated
native sessions; an active transaction owns one native session on the lane.
There is no new MySQL wire protocol, retry or session-pooling claim.

M3a intentionally selects the released synchronous prepared ORM path on the
bounded serial worker (including transactions). CFlow asynchronous guest
delivery remains; hard cancellation of already-entered native network I/O
and owner-scheduler native async query progress are not yet qualified.
Native driver timeout is configured explicitly. Failed/unknown outcomes
retain the existing no-replay and checked cleanup contract.

Real MySQL Core Wasm interpreter/MIR CTests on Linux x64 use the Ubuntu 24.04
runner's preinstalled MySQL 8.0 service, bound to loopback with required TLS,
a short-lived generated P-256 CA/server certificate, separate restricted
writer and SELECT-only accounts, and isolated fixture tables for the two
executors. No external database installer or production credentials. Tests
require correct server identity, reject a wrong TLS identity, verify
prepared binary insert, exact typed SELECT metadata/row bytes, read-only
denial, transaction-local visibility, independent connection visibility,
commit, rollback, duplicate-key quarantine, stale tokens, and abandoned
transaction/cursor shutdown. Independently check the final committed rows
using the local MySQL client. Missing fixture/service configuration fails
rather than skips. Other desktop profiles run the unchanged SQLite
qualification and compile the common native SQL provider; Android
cross-builds it. The runner controls the installed server patch version;
the TurboDB SDK is still digest-pinned. No cross-platform MySQL live
qualification or lost-COMMIT fault injection is claimed by this slice.

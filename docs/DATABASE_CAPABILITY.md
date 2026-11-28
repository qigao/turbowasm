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

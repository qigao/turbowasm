# TurboWasm

TurboWasm is a small C11 WebAssembly runtime with explicit semantic, sandbox,
and backend boundaries.

## Current runtime

The repository currently provides:

- bounded WebAssembly binary reading and module admission;
- retained validation metadata for sections, control signatures, functions,
  data/element segments, tables, memory, references, bulk operations and SIMD;
- a semantic-reference interpreter with structured control, direct/indirect
  calls, memory/table/reference/bulk operations, SIMD, tail calls and typed
  exception handling;
- proposal-aware multi-memory, custom page sizes, extended const, relaxed
  SIMD and memory64 semantics with pinned upstream qualification;
- Core 3.0 recursive types/subtyping, typed function references, GC structures/arrays,
  explicit store/root ownership and table64 addresses;
- typed tag/exception identity and cross-frame unwind across direct, indirect,
  imported and tail-call boundaries;
- shared fuel and interruption semantics across interpreted and compiled
  execution, plus restartable interpreted/native execution with fuel/interruption/
  host-wait yields;
- scalar value kinds plus typed `v128` lane refinement;
- CMeta-backed vector/mask semantic descriptors;
- portable SIMD execution through `Salts::SIMD`;
- an optional lazy MIR JIT for eligible hot functions;
- complete scalar numeric MIR admission, including comparisons, conversions,
  saturation and exact floating constants, sharing Runtime numeric primitives;
- helper-backed SIMD MIR lowering with invocation-local private `v128` slots,
  including structured control flow;
- an explicit MIR executable-mapping budget;
- typed cross-instance linking for functions, globals, memories, tables and
  tags, plus typed synchronous host-function providers;
- optional `TurboWasm::CFlow` deadline/scheduling projection;
- optional `TurboWasm::NativeIO` bounded async host-wait bridge;
- shared-memory/atomic execution with wait/notify and qualified legacy WASI
  threads support;
- a capability-gated `TurboWasm::WASI` Preview1 layer for args/environment,
  clocks, random, fd I/O, proc_exit and filesystem operations;
- a bounded generation-safe WASI filesystem provider ABI with Salts HostFS and
  optional littlefs implementations;
- optional `TurboWasm::WASINativeIO` async fd projection and
  `TurboWasm::WASIThreads` CFlow-backed thread-spawn/group lifecycle, with
  borrowed executors or `turbowasm_wasi_threads_init_pool` for an owned pool
  reserving one worker per admitted child (the root runs outside that pool);
- caller-owned Runtime allocation plus module/allocation/linear-memory/table
  resource limits for embedded deployments;
- C/C++ public ABI tests;
- an installed CMake package, module-validation CLI and WASI command runner;
- an optional local Metallic wasm32 guest SDK for C11 applications.

The current implementation is intentionally scoped. Core shared-memory atomics,
legacy WASI threads, the qualified Preview1 capability layer, and interpreter
memory64 are part of the completed Runtime surface. Shared memory64 supports
atomic operations, wait/notify, imports and guarded growth. MIR admits unshared
memory64 modules and lowers scalar load/store, size/grow, bulk memory and its
SIMD memory instructions through Runtime helpers. Shared memory32/memory64
also use these helpers, including atomic load/store, RMW, compare-exchange,
fence and interruptible wait/notify. The private entry
accepts mixed scalar/reference/vector parameters and zero or multiple results.
Direct, table-indirect and typed-reference calls and their tail forms support these tuples
through Runtime, including interpreted callees and imports admitted by the
existing host-signature API. Tail dispatch
reuses the logical call depth. Reference locals, control merges and call scratch
use complete value cells rooted in the owning store. Vector call cells preserve
bits and shape through invocation-owned SIMD slots. Indirect targets retain Runtime's table bounds,
null and subtype checks; cross-instance tails switch owners after the native
frame unwinds. Each instance/backend retains one execution owner,
while distinct instances can import the same shared backing. Native MIR
memory64 verification passed the Linux and macOS MIR test profiles; see the
[qualification record](tests/conformance/README.md). Windows x64 uses the shared
`mir-jit` port revision 3, and `ci-win-user` enables the MIR test graph.
The Component Model host API is exposed separately
through the optional `TurboWasm::Component` façade; it does not enter the
`TurboWasm::Runtime` public ABI. WASI 0.2 remains a separate capability layer
tracked by Runtime v2 (#301). The interpreter supports GC and all table64
operations. MIR also lowers table.size, table.init, table.copy, elem.drop and
table.get/set/grow/fill
through Runtime for table32/table64, including mixed-width and imported tables.
MIR lowers the complete GC instruction family through Runtime: struct/array
construction and access, segment operations, casts/tests and cast branches,
external-reference conversions and i31. Constructor operands and intermediate
references use the native frame's rooted value cells; store quotas and collection
remain Runtime-owned. MIR also lowers typed exceptions with lexical catch
dispatch, Runtime tag identity/payload ownership and mixed-tier unwind. Native EH
passed Linux/macOS MIR and the complete pinned Core 3.0 differential suite.

MIR also executes scalar/vector/reference global reads and writes through Runtime,
including imported globals, nullable-reference branches and `unreachable` traps.
Provider state, reference owners and GC roots follow the same Runtime contracts.

Restartable executions use the instance's tiering policy when the attached
backend explicitly supports suspension. MIR retains native frames through fuel,
interruption and host-wait yields; GC roots and vector slots remain live until
the call completes or cancellation unwinds it. Host effects are not replayed.
The instance, module, providers and backend must outlive the execution handle.

The synchronous Component layer supports UTF-8, UTF-16LE and compact
Latin-1/UTF-16 canonical strings, including nested host values and memory64.
`post-return` callbacks run after result lifting and share the
resumable call's fuel/interruption control; failed cleanup discards unpublished
results. Direct canonical cleanup targets are type checked and enforce the same
leave restrictions as indirect calls from Core cleanup. Canonical lower and
resource creation/deletion trap during cleanup or guest realloc.
Guest resource destructors inherit the invoking call's fuel, interruption,
host-wait ownership and call-depth limit. Consumed handles stay consumed if
destruction traps or the suspended call is cancelled.
Guest realloc reached during canonical lowering shares that control too; failed
or cancelled result conversion restores the leave gate without publishing a
partial host result.
Local-function canonical lowering shares these adapters and resource ownership
rules, including aliases, Core start calls, nested composite values and string
conversion across memory32/memory64. Large parameter tuples use the canonical
indirect ABI with aligned, checked memory access and a separate result pointer.
The Component host API also exposes explicit async loading, bounded instances,
task driving and cancellation, authenticated host-wait completion, typed
future/stream endpoint moves, transfer results and cooperative shutdown.
See [component.h](include/turbowasm/component.h) for the ownership/error contract
and [the async interface](ARCHITECTURE.md#public-async-c-interface) for examples.
Async tasks and transfers consume owned leaves only through explicit move entry
points; returned storage remains charged until actual destruction. Existing
synchronous admission retains its behavior. Component provider linking, nested
instantiation and broader WASI 0.2 integration remain incomplete; the Component
Model is not yet complete.

WASI 0.2 TCP has a provider-neutral state machine and an optional installed
CNet backend covering connect/accept, byte streams and reusable readiness.
See [native TCP setup](#native-wasi-02-tcp) and the
[socket backend design](ARCHITECTURE.md#wasi-socket-backend-and-reusable-io-readiness).
WASI UDP message streams and asynchronous name lookup use additive versioned
providers and the same optional CNet backend. See [UDP and DNS setup](#native-wasi-02-udp-and-dns).

MIR admits the complete helper-backed SIMD instruction set, including shuffle,
lane extraction/replacement, extending/splat/zero loads and lane loads/stores.
Immediate SIMD operations reuse Runtime's validated instruction semantics; this
does not introduce a platform vector calling convention or MIR vector registers.

## Core 3.0 and managed references

The interpreter passes the complete pinned [Core 3.0 suite](tests/conformance/README.md):
258 files, 63970 binary commands, zero failures and zero unsupported commands.
The 1229 text-syntax assertions are checked by wasm-tools and reported separately;
TurboWasm itself accepts binary modules. This qualification does not imply full
Component Model, WASI 0.2, or native JIT coverage.

Modules with GC operations or composite types use `turbowasm_instance_create_in_store`.
Create one `turbowasm_store` on its owner thread and use it for the consumer and
all instance providers linked to it (host-function bindings need no instance). Ordinary instance creation rejects these modules
with `TURBOWASM_INVALID_ARGUMENT`. Existing non-GC creation remains available.

Returned GC values borrow their object until the next allocation/collection in the
store. Call `turbowasm_root_retain` before keeping a value across that boundary and
`turbowasm_root_release` when done. Copying `turbowasm_value` does not retain it.
Globals, tables, element segments, active/suspended frames and execution results
are traced automatically. Destroy executions and instances, release host roots,
then destroy the store. Function/exception references inside GC objects still
borrow their original owner instances. All store operations use the owner thread;
shared-memory threads do not enable shared GC execution.

See the complete, installed-package-tested [GC example](examples/gc.c) and
[store API](include/turbowasm/store.h). It retains a struct across instance/module
destruction and collection. To build the example independently, set
`CMAKE_PREFIX_PATH` to the installed TurboWasm and Salts SDK roots when configuring
`examples/CMakeLists.txt`.

Store defaults are finite: 64 MiB of requested metadata/payload bytes, 65536
objects, and 4096 root registrations (host roots plus active frames). Override
these through `turbowasm_store_config`; exhaustion returns `OUT_OF_MEMORY`.
Table64 accessors preserve all 64 address bits. Dense table storage is limited to
`UINT32_MAX` elements and `runtime.limits.max_table_elements`; growth beyond the
limit returns the Wasm failure sentinel without changing the table.

Rebuild consumers for the expanded public value/API surface. Artifact schema 2
retains GC type groups, store requirements and 64-bit table limits; regenerate
schema 1 artifacts from source.

## Native WASI 0.2 TCP

`TurboWasm::WASI02IO` supplies bounded shared stream/poll readiness.
`TurboWasm::WASI02CNet` adds externally driven TCP, including connect/accept,
portable addresses/options, reusable subscriptions, streams and half-close.
Its public header is `<turbowasm/wasi02_cnet.h>`; the provider-neutral facade
and Runtime retain their existing dependency boundaries.

Enable `TURBOWASM_ENABLE_WASI02_SOCKET_BACKEND=ON` with a Salts SDK exporting
`cnet_connection_preserve_send_on_eof`, datagram socket controls and public
name lookup, plus `Salts::IDNA` with `SALTS_IDNA_UNICODE17_UTS46_35_DNS` for
Unicode names. The DNS profile is an upstream addition after Salts 2.3.0-rc.2;
that release alone cannot build this backend. The option defaults off for local profiles; native CI
presets enable it with the published SDK, including the Metallic guest profiles.
The runtime+component-only SDK and Core conformance profiles explicitly keep it
off. Requesting it with an unsuitable SDK fails configuration.

Initialize zeroed I/O and CNet owners, then compose the facade with
`turbowasm_wasi02_cnet_wasi02_init` and create the Component instance with
`turbowasm_wasi02_component_instance_create_async`. CNet config helpers set
finite bounds and deny bind/connect/accept by default; explicitly enable the
required operations and supply an optional additional address policy.

One host owner drives progress in this order:

1. `turbowasm_wasi02_cnet_advance` and `..._next_timeout` prepare pending work.
2. Observe the caller-owned NativeIO backend once; route every completion with
   `..._route_completion`. Forward unconsumed completions to their actual owner.
3. Advance CNet and the shared I/O domain, then explicitly resume yielded calls.

Drop calls/instances and facade resources, request adapter shutdown, continue
observing/routing until `..._shutdown_poll` reports complete, then destroy the
adapter and I/O domain before destroying the backend. Child streams remain
usable after their parent socket is dropped; poll aliases retain readiness
metadata and dropping a subscription does not cancel transport.

The installed TCP consumer test performs real async WIT ping/pong through
memory32 and memory64, plus IPv4/IPv6, backpressure, flush, inherited options,
half-close and retained-carrier shutdown. UDP/DNS tests cover message boundaries,
empty packets, ordered address results and retained subscriptions. See
[the socket design](ARCHITECTURE.md#wasi-socket-backend-and-reusable-io-readiness).

For branch SDK qualification, dispatch TurboWasm CI with `salts_ci_run` set to
a successful Salts SDK preparation run. CI verifies the artifact's source
commit, builds the full native matrix and runs the existing installed-package
tests. The override selects the dependency only; native CI presets enable socket
coverage for both prerequisite artifacts and ordinary published SDKs.
Set `skip_windows=true` for an explicitly partial qualification of Linux,
macOS and Android. In that mode, each prerequisite Salts platform job must
have completed successfully and its SDK source commit must match the run;
an unfinished Windows job does not block those platforms. The CI summary
records the omission, and this run does not qualify Windows.

## WASI Preview1 sockets and polling

`TurboWasm::WASI` implements `sock_accept`, `sock_recv`, `sock_send`,
`sock_shutdown`, `fd_fdstat_get`, `fd_fdstat_set_flags`,
`fd_fdstat_set_rights` and `poll_oneoff` in `wasi_snapshot_preview1`.
The existing file descriptor table also routes socket `fd_read`, `fd_write`
and `fd_close`. These are memory32 Preview1 imports; native socket creation,
bind, connect and DNS remain explicit host capabilities.

Include `<turbowasm/wasi_sockets.h>` and opt in with
`turbowasm_wasi_preview1_init_v2`. The old initializer remains available.
The table is borrowed and must outlive WASI and every consumer instance.

```c
turbowasm_wasi_preview1_config_v2 config;
turbowasm_wasi_preview1_config_v2_init(&config);
config.base.filesystem = &filesystem; /* already initialized common fd table */
config.base.allow_fd_read = config.base.allow_fd_write = true;
config.allow_sockets = config.allow_poll = true;
/* Check status; clock subscriptions require a separate base.allow_clock grant
 * and base.clock_time provider. Defaults do not grant either capability. */
turbowasm_status status = turbowasm_wasi_preview1_init_v2(&wasi, &config);
```

Defaults bound simultaneous waits to 64, subscriptions per poll to 64,
bytes per socket call to 65536 and all staged call storage to 4 MiB.
Capacity exhaustion returns `AGAIN` (waits), `NOMEM` (staging), `MFILE`
(descriptors), or `MSGSIZE` (per-call bytes). The facade never blocks its
owner thread. Use `turbowasm_execution_resume`; after progressing providers,
call `turbowasm_wasi_preview1_advance` to complete readiness waits and resume
the execution. One-shot invocation returns `AGAIN` when it would need to wait.
`next_timeout` reports the next timer delay in nanoseconds, or `UINT64_MAX`.

`TurboWasm::WASICNet` composes the existing CNet owner with Preview1; it adds
no worker or backend observation loop. Include `<turbowasm/wasi_cnet.h>`,
create host TCP/UDP carriers through the existing CNet providers, then move
them using `turbowasm_wasi_cnet_listener_move`, `tcp_move` or `udp_move`.
The CNet adapter must have no published facade bindings or stream/poll/error
aliases during transfer. TCP moves its socket/input/output together; UDP
requires an associated peer and moves its socket/datagram pair together.
Moves zero carriers only on success and invalidate old stream-domain tokens.

Obtain operations with `turbowasm_wasi_cnet_descriptor_ops`, then admit the
owned file using `turbowasm_wasi_fs_bind_socket_move` with its socket type,
explicit rights and optional `TURBOWASM_WASI_FDFLAG_NONBLOCK`. If fd admission
fails, the file remains host-owned; retry or close it through `ops.file.close`.
A successful bind transfers close ownership to the common table.
All APIs run on the same owner thread. The host observes the existing native
backend once, routes every completion to CNet, progresses CNet and its IO
domain, then advances Preview1. Files and sockets share fd allocation and
can use different copied providers.

Receive supports `PEEK` and `WAITALL`; UDP retains one packet boundary and
reports `DATA_TRUNCATED`, including empty messages. TCP PEEK|WAITALL reserves
another `receive_bytes` from the CNet payload budget at admission and accepts
at most `receive_bytes` per peek. Ordinary WAITALL uses bounded facade staging.
Only one active operation per descriptor and direction is admitted (`BUSY` on
conflict). Rights can only decrease; accept uses the listener's current
inheriting rights, restricted to stream capabilities.

Closing a descriptor invalidates its guest identity immediately after provider
close succeeds. Advance Preview1 to wake its waiters; leases prevent old calls
from accessing a new fd with the same number. Execution destruction unwinds
waits and releases staging. `shutdown_request` wakes calls with `INTR`;
`shutdown_poll` confirms they have unwound. Destroy consumer instances before
`destroy_checked`, close remaining fds and drain CNet's actual native terminals
before destroying its owner, IO domain and backend.

The executable tests in `tests/wasi_preview1_sockets_test.c` and
`tests/wasi_preview1_cnet_test.c` use the checked-in Core fixture
`tests/fixtures/wasi_preview1_sockets.wat`; the native test executes real IPv4
and IPv6 TCP/UDP. The installed package runs the same public-API tests plus a
C++ consumer. MIR profiles attach a native backend to the fixture and verify
that executed import wrappers compile.

## Native WASI 0.2 UDP and DNS

Include `<turbowasm/wasi02_network.h>` for the complete provider-neutral
`udp`, `udp-create-socket` and `ip-name-lookup@0.2.8` bundles. Existing TCP
provider/config layouts remain unchanged. Versioned config helpers initialize
finite defaults; zero facade capacities omit the corresponding capability.

For CNet, use `turbowasm_wasi02_cnet_init_external_v2` followed by
`turbowasm_wasi02_cnet_wasi02_init_v2`. The optional `TurboWasm::WASI02CNet`
target requires a Salts SDK exposing datagram socket controls and
`<cnet/name_lookup.h>` and `Salts::IDNA`, in addition to directional TCP EOF.
The adapter applies Salts' Unicode 17 / UTS46 nontransitional validation before
name authorization, then passes the same ASCII identity to CNet's c-ares
progress owner. Numeric addresses and optional DNS root dots remain supported;
ICU and SaltsUtils are not required. Guest CMeta uses the Salts `2.3.0-rc.2`
sources; the host SDK additionally requires the post-rc.2 IDNA DNS profile
described in [the absolute-name migration](ARCHITECTURE.md#absolute-dns-names-after-uts46-mapping).

```c
turbowasm_wasi02_cnet_config_v2 native;
turbowasm_wasi02_config_v2 facade;
turbowasm_wasi02_cnet_config_v2_init(&native);
turbowasm_wasi02_config_v2_init(&facade);
native.allow_udp_bind = native.allow_udp_send = true;
native.allow_udp_receive = native.allow_name_lookup = true;
facade.base.socket_network_resource_capacity = 16;
facade.base.tcp_socket_resource_capacity = 16;
facade.base.stream_resource_capacity = 32;
facade.base.pollable_capacity = 64;
facade.udp_socket_capacity = 16;
facade.datagram_stream_capacity = 32;
facade.resolve_stream_capacity = 16;
```

Apply the desired address/name restrictions before initializing the adapter.
The borrowed backend must cover `2*T + U` endpoints and
`3*T + U*(send_datagrams+1)` requests, where `T` and `U` are configured TCP/UDP
capacities. DNS deadlines join `..._next_timeout`; use the same progress and
shutdown sequence described above. Numeric literals never issue DNS requests.

Datagram receive returns bounded message records, including empty messages.
Send requires a fresh check-send grant and reports the exact admitted prefix;
a later failure appears on the next check. Drop both old datagram streams before
changing the UDP association. A concurrency-conflict requires routing retained
terminals before retrying. Closed poll aliases retain their original metadata
and count against the configured pair capacity. Name streams return pending,
ordered addresses, EOF or an exact resolver error; cancel/drop keeps in-flight
c-ares storage until its real terminal. External DNS is unnecessary for tests.

CNet's [WebSocket API](../salts/cnet/include/cnet/websocket.h) remains available
to hosts. WASI sockets 0.2.8 does not define a WebSocket interface.

## Local C11 guest SDK

The optional C11 guest SDK uses the local sources in `guest/metallic`. It performs
no Metallic download during configure or build. Origin, revision, license and
local changes are recorded in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

Build with LLVM 20+ `clang`, `llvm-ar` and `wasm-ld` on PATH and a Salts SDK providing
`SALTS_FS_ROOT_MUTATION_VERSION` (secure rename, exclusive create and append):

```powershell
# Run from a Visual Studio developer shell with the normal SDK environment.
cmake --preset win-metallic-user
cmake --build --preset win-metallic-user
ctest --preset win-metallic-user -R guest --output-on-failure
build/win-metallic-user/turbowasm-run.exe build/win-metallic-user/guest/guest_hello.wasm demo
```

`turbowasm-run [--dir absolute-host-directory] [--env KEY=VALUE] [--fuel N]
[--memory-pages N] [--] module.wasm [arguments...]` invokes the exported `_start`.
The default limits are 16 MiB linear memory, 16 MiB module bytes, 64 descriptors,
65,536 table elements and 100 million instructions. Native blocking stdio is not
a wall-clock deadline. No directory or environment is inherited; `--dir` grants
read/write access beneath that root and maps it to guest `.`. HostFS never follows
guest symlinks. The runner rejects command modules with a separate start section
so instantiation cannot run unmetered guest code. Runtime failures return process
status 125; guest `proc_exit` returns its low eight bits.

`cmake --build --preset install-win-metallic-user` installs the runner and guest
headers, `metallic.a`, `crt1.o`, `crt1-reactor.o`, license and the guest CMake helpers. Installed CMake
consumers can include `${TurboWasm_GUEST_SDK_DIR}/TurboWasmGuest.cmake` after
`find_package(TurboWasm CONFIG REQUIRED)`, then call
`turbowasm_add_c_guest(app /absolute/path/app.c)`. This emits `app.wasm` with a
256 KiB shadow stack and 16 MiB declared maximum memory. LLVM versions must be
compatible with the LTO archive used by that SDK.

The opt-in `TURBOWASM_BUILD_METALLIC_THREADS` option also installs
`threaded/include`, `threaded/lib/metallic-threaded.a`, separate command/Reactor
CRTs and profile metadata beneath the guest SDK directory. It requires the guest
SDK and WASIThreads adapter; the Metallic qualification presets enable it.
Existing helpers remain single-threaded unless explicitly passed `THREADS`:

```cmake
turbowasm_add_c_guest_library(worker_code THREADS SOURCES worker.c)
turbowasm_add_c_guest(app "${CMAKE_CURRENT_SOURCE_DIR}/main.c" THREADS
  LIBRARIES worker_code)
turbowasm_add_c_reactor(tool THREADS SOURCES tool.c
  EXPORTS tool_step tool_close LIBRARIES worker_code)
# After including TurboWasmGuestCMeta.cmake:
turbowasm_add_cmeta_guest_library(guest_cmeta /absolute/path/salts/cmeta THREADS)
```

Threaded programs require library targets carrying the matching guest profile;
mixed single/threaded targets and unannotated absolute archives are rejected.
All objects must use the matching Clang/LTO, native TLS and standard Wasm EH/SJLJ
configuration. The output imports shared `env.memory` (initial 1 MiB, maximum
16 MiB), exports `wasi_thread_start`, and keeps a 256 KiB root shadow stack.
The host supplies one shared memory provider to the entire group, calls the
budgeted creation API, then invokes `_start` or `_initialize` once. Do not send
these modules to the single-thread `turbowasm-run` CLI. See the
[threaded embedding session](examples/guest/threaded_session.h) and run
`turbowasm_threaded_reactor_example path/to/guest_threaded_counter.wasm` for a
persistent worker example. These are embedding examples, not an installed host
session ABI.

The C11 thread profile provides lifecycle, plain/recursive/timed mutexes,
conditions, once and TSS. It bounds guest child records at 32, each with a 128 KiB
stack and up to 64 KiB TLS; retained joinable results count against that bound.
There are 128 TSS keys and four destructor passes. Host admission can impose a
smaller capacity. Prefer `turbowasm_wasi_threads_init_pool`: the root runs outside
its pool and every admitted child has a worker available even during blocking
join/wait. Capacity exhaustion returns `thrd_nomem`, while stale handles and
closed admission return `thrd_error`. These limits do not prevent application
lock cycles. Timed calls require a working realtime clock provider.

Guest `<metallic/threads.h>` adds `metallic_threads_close(utc_deadline)`: root-only,
permanently closes spawn admission and waits for stack-free child terminals.
NULL waits without a deadline; an absolute `TIME_UTC` deadline can return
`thrd_timedout`. Invalid deadlines or clock failure return `thrd_error`.
Timeout/error retains live storage and permits retry; a wrong-thread call
leaves admission unchanged. Stop application work before close, then release
application objects afterwards. Close does not flush FILEs or run exit handlers.
Main return/`exit` still terminates the process group, without an implicit join.
After root/child failure, skip guest cleanup, request group exit and keep all
providers, memory and modules alive until actual child termination. If sockets
need owner dispatch, continue owner progress while workers execute or drain.
One root call is admitted at a time. Shared managed GC and pthread compatibility
are outside this profile; CMeta reflection does not synchronize user objects.

For a persistent application, use `turbowasm_add_c_reactor(tool SOURCES tool.c
EXPORTS tool_step tool_close)`. The host calls `_initialize` once, then retains
the instance between business calls. Initialize before any allocation or
business call; duplicate initialization traps. Close explicitly to release
application objects and flush output. The [counter example](examples/guest/counter.c)
and [embedding session](examples/guest/reactor_session.h) demonstrate bounded
calls, exclusive admission, and terminal teardown after traps or fuel exhaustion.
Run `turbowasm_reactor_example path/to/guest_counter.wasm` for the native example.

`turbowasm_add_c_guest_library(name SOURCES a.c b.c)` emits a guest archive.
Program helpers accept `LIBRARIES` with guest library targets or absolute archive
paths; all helpers accept `INCLUDE_DIRECTORIES` and `COMPILE_OPTIONS`. The output
variables are `<name>_ARCHIVE` and `<name>_WASM`. Invalid arguments or native
library targets fail configuration; missing source files fail the build.

To cross-compile CMeta core from local Salts source, include
`${TurboWasm_GUEST_SDK_DIR}/TurboWasmGuestCMeta.cmake` and call
`turbowasm_add_cmeta_guest_library(guest_cmeta /absolute/path/salts/cmeta)`.
Pass that guest target in `LIBRARIES` and its `cmeta/include` directory in
`INCLUDE_DIRECTORIES`. The source needs the Wasm C11 `aligned_alloc` path in
`cmeta/src/data.c`. Set the parent environment variable
`TURBOWASM_GUEST_CMETA_SOURCE_DIR` before configuring a Metallic user preset to
enable the project's CMeta guest tests. This compiles portable core sources,
excluding CMetaNative; host SDK archives cannot be linked into Wasm. Metadata,
function pointers and object ownership stay within the guest instance.

`setjmp`/`longjmp` use LLVM SJLJ lowering and standard Wasm exception handling.
The helpers supply compiler and LTO flags together, including the Wasm exception
model; the small libc SJLJ support objects stay outside LTO. Rebuild all guest
objects using the new `jmp_buf` layout. Omitting lowering fails to link rather
than returning a false success. A jump target must still be active on the same
C thread; returning from a Reactor export ends that target's lifetime. Jumps do
not perform application cleanup and must not bypass a CMeta scope, live host
callback or async boundary. See the [design and references](ARCHITECTURE.md#metallic-non-local-jumps-427).

The profile supports command args/environment, stdio, ordinary files,
rename/remove, exclusive creation, temporary files, append mutation, allocation,
exit handlers and realtime `timespec_get`. Temporary files require a writable
preopen and are unlinked immediately. Stdio defaults to unbuffered operation.
Before I/O, `setvbuf`/`setbuf` can select full or line buffering; a supplied buffer
must remain alive until the stream closes or reopens. With a null buffer, libc
allocates the requested capacity (BUFSIZ for size zero), and frees it on close or
reopen. Invalid settings and allocation failure return nonzero with errno set.
`fflush(NULL)` and normal exit flush all buffered outputs; `quick_exit` and
`_Exit` do not. Flush failures set the stream error indicator and retain the
unwritten suffix for a `clearerr`/`fflush` retry. Positioning accounts for input
read-ahead and output buffering, and successful seek discards pushback and EOF.
`freopen(NULL, ...)` permits append changes with unchanged read/write access;
truncation or access changes return `ENOTSUP` and close the stream. Threads,
full locale/fenv and the documented upstream
long-double gaps remain outside this profile; it is not complete C11 conformance.
The separate C11 threads profile is exercised by
`turbowasm_guest_c11_threads_test`, including synchronization, TSS, allocation
and shared Preview1 clock/args/environment/vector I/O. Clang supplies C11
language support and `stdatomic.h`; Metallic supplies the guest C library.
Compiled tests cover 8/16/32/64-bit integer atomics, pointer arithmetic, CAS,
flags, fences and release/acquire publication with real guest workers. This
does not qualify arbitrary aggregate or wider-than-64-bit atomics.
Its libc tests also cover
per-stream byte/wide I/O serialization, close/reopen against flush-all, independent
stream progress, exit callback registration, random state, signal handlers,
thread-local string/calendar buffers and once-only preopen discovery. Filesystem
callbacks in these libc tests are controlled providers; they do not establish
concurrent HostFS support by themselves. Separate native filesystem tests cover
table admission/close races, per-file vector I/O and directory cursor ordering,
bounded HostFS slot reservation and rename/open exclusion. A separate compiled
Metallic guest runs four C11 workers against production HostFS, covering create,
append, seek, read, stat, rename, remove and temporary-file cleanup. Shared
Preview1 paths/random use bounded snapshots; fixed fdstat output uses protected
copies. Concurrent close can return `BUSY` while synchronous calls are in flight;
callers retry after those calls finish. Socket/poll also uses protected copies
and owned arguments across callbacks and waits, with the existing v2 capacity
limits. The v2 initializer still requires one host progress thread. The opt-in
`turbowasm_wasi_preview1_init_threaded` initializer admits guest worker calls:
a bounded queue transfers socket callbacks to the initializing owner, while
ordinary files remain concurrent. Workers can interrupt readiness waits through
their Runtime invocation policy; cancellation retains borrowed state until the
owner acknowledges it. The host must keep advancing native transports and
Preview1 while workers run or drain, then close sockets before destroying WASI.
Compiled C11 guests exercise TCP/UDP and poll while the root joins children,
including group `proc_exit`. The same formal threaded command/Reactor/CMeta
consumer suite runs against the build tree and installed SDK. It covers
constructor/TLS identity, recursive/duplicate CRT, close timeout/error/retry,
detached children, root admission, fuel/interruption and group exit/failure.
Final platform and acceptance qualification is tracked in
[#426](https://github.com/qigao/turbowasm/issues/426). Native ASan instrumentation
does not instrument guest C code or prove freedom from guest data races;
ThreadSanitizer qualification has not been performed.

Hosts accepting modules with a Wasm start section can use
`turbowasm_instance_create_linked_with_options` to apply fuel and interruption
to startup. The options apply only to start; later calls select their own
policy. Failure empties the destination instance but does not undo imported
state or host effects. WASI child startup shares the spawning invocation's
remaining budget and interruption context. Existing creation APIs retain their
behavior, and the single-thread Reactor example still rejects start sections.
When the local CMeta source is configured, the internal threads test also builds
a separate guest CMeta archive and runs four C11 workers through cross-TU
metadata, checked calls, independent ObjectRef lifetimes and aligned allocation.
Immutable descriptors are shared; the test's mutable counters and objects use
TLS. This qualifies independent objects, not concurrent mutation of one object.
The guest CI profiles are `ci-metallic-user` (Linux) and
`ci-macos-metallic-user` (macOS); manual CI selects both with
`metallic_guests=true`. Salts `2.3.0-*` remains the published SDK selector; native
socket builds require the new IDNA DNS profile. Until that profile is published,
use `salts_ci_run` with its qualified prerequisite SDK artifact.
Its parent `TURBOWASM_GUEST_LLVM_ROOT` selects the guest compiler tools explicitly;
CI supplies LLVM 21 and also enables MIR and mixed-tier guest regression.
macOS uses AppleClang for native SDK ABI compatibility and installs LLVM/linker 21
separately for guest compilation. Both platforms run the installed consumer suite.

The guest tests also cover restartable UTF-8/UTF-16/UTF-32 conversions: split
sequences, independent implicit state, null-input semantics, surrogate pairing,
invalid prefixes and bounded wide-string conversion. Numeric regression uses a
64-bit bit-by-bit oracle for all 128 shift counts, integer conversion boundaries
and fixed binary128 division results (including subnormal ties). These bounded
checks do not replace the upstream complete math/oracle suite.
Buffering tests cover visibility, positioning, normal/quick/immediate exit,
buffer lifetime, allocation failure, and scripted short I/O/error recovery at
the guest's WASI import boundary.

## Dependency boundary

TurboWasm does not depend on SaltsUtils. The Preview1 ABI plan in
`src/wasi_preview1_adapter_plan.h` is maintained with its CMeta declarations in
`src/wasi_preview1.c`; `turbowasm_wasi_adapter_plan_test` checks every function
and parameter against those declarations. The external DataBind generator and
byte-for-byte regeneration check have been removed in favor of this existing
semantic test. Remove `TURBOWASM_QUALIFY_WASI_ADAPTER_PLAN` and
`-WithSaltsUtils` from older build invocations; use `ci-win-user` instead of
`ci-win-qualify-user`. No runtime or guest API changes are required.

```text
qigao/vcpkg-cache
    -> SIMDe package cache
    -> MIR JIT package + executable-memory limit

Salts SDK selected by package acquisition
    -> CMeta
    -> Salts::SIMD (SIMDe is private)
    -> Salts::Coroutine (private resumable Runtime implementation)
    -> optional Salts::CFlow / Salts::NativeIO adapters

TurboWasm::Runtime
    -> Wasm decoding / validation / sandbox semantics
    -> retained typed validation metadata
    -> interpreter + restartable execution
    -> optional lazy MIR JIT

TurboWasm::CFlow / TurboWasm::NativeIO
    -> optional host scheduling / async-I/O projections
    -> do not enter the Runtime public link interface

TurboWasm::WASI / WASIThreads / WASINativeIO / WASIHostFS / WASILittleFS
    -> optional host capability layers
    -> remain separate from TurboWasm::Runtime

TurboWasm::Component
    -> optional synchronous Component Model façade
    -> public component.h is opt-in and is not included by turbowasm.h
    -> depends on TurboWasm::Runtime, never the reverse
```

TurboWasm never includes SIMDe directly and does not expose MIR types through
the installed `TurboWasm::Runtime` target.

## Build

Use CMake 3.25+, Ninja, a Salts SDK selected by `2.3.0-*`, and the shared
[qigao/vcpkg-cache](https://github.com/qigao/vcpkg-cache) toolchain. The Salts
2.x API uses `cmeta_v128`, `cmeta_simd_*`, `cmeta_*` platform/filesystem
functions, and `<coro.h>`. TurboWasm does not pin the SDK version or provide
aliases for the removed Salts names.

`CMakeOptions.cmake` owns feature defaults. The versioned `CMakeUserPresets.json`
provides configure/build/test/install entry points; `presets/` contains shared
base settings. Third-party dependencies use the root `vcpkg.json`: enabling
`TURBOWASM_ENABLE_MIR_JIT` selects `mir`, while conformance selects host WABT.
The shared NuGet feed is read-only and the local binary cache is writable.
Provide `GITHUB_TOKEN` with package read access in the parent environment;
presets do not load `.env`.

For local SDK acquisition, run the following in PowerShell with .NET SDK 8+
on `PATH` (the runtime alone is insufficient):

```powershell
./cmake/ci/restore-salts-sdk.ps1 -Local -SaltsRid windows-x64
```

This resolves `Salts.Native` using `Version="2.3.0-*"` with
`--no-cache --force-evaluate` and exports
`SALTS_ROOT` in that PowerShell process. Alternatively, set `SALTS_ROOT` to an
already installed release SDK. SaltsUtils is not required.

Set `PROJECT_ROOT` to the parent containing `external/pkgs`, and `VCPKG_ROOT`
to the vcpkg checkout. Windows expects the cache checkout at
`%LOCALAPPDATA%/qigao/vcpkg-cache`; Linux uses `VCPKG_CACHE_REPOSITORY_ROOT`
and requires Mono for NuGet restore. SDK lookup is restricted to `SALTS_ROOT`;
`CMAKE_PREFIX_PATH` contains only the matching vcpkg profile.

From a Windows VS developer environment (`VsDevCmd.bat -arch=x64 -host_arch=x64`):

```bat
cmake --preset win-release-user
cmake --build --preset win-release-user
ctest --preset win-release-user
cmake --build --preset install-win-release-user
```

On Linux, use `linux-release-user` for the same four commands. The `win-dev-user`
and `linux-dev-user` presets select Debug and require a matching Debug Salts SDK
in `SALTS_ROOT`. Build and vcpkg installed trees are separate per profile;
installation uses `$PROJECT_ROOT/external/pkgs/turbowasm/debug|release`.
To select the optional MIR backend locally, set `TURBOWASM_ENABLE_MIR_JIT` in
the selected user preset before configuring; no manual MIR prefix is needed.

Windows CI uses `windows-2025` and the Windows triplet supplied by the shared
cache action, matching its compiler/SDK contract.

CI and native SDK releases share `.github/workflows/native-build.yml` and use
`ci-*-user` presets. `cmake/ci/select-ci-scope.ps1` owns the platform matrix:
code changes and manual CI runs select all five qualification profiles by default;
prose-only PRs skip native builds and still report `CI result`. SDK preparation
selects the four shipping platforms from the same definitions. Manual SDK runs
build package artifacts; publication remains restricted to release tags.
The shared workflow uses Mozilla's [sccache Action](https://github.com/mozilla-actions/sccache-action)
for C/C++ compiler caching. `actions/cache` restores and saves a bounded 1 GiB
local cache per OS, architecture and preset, with independent writer keys and
hit/miss statistics in each job. This avoids the per-object GHA backend writes
that failed during full-graph qualification. The compiler launcher comes from
the CI environment through the user presets; ordinary local builds do not
require sccache. vcpkg binary caching remains independent.
For published Salts SDKs, including 2.2.0, the build and installed-package tests
normalize the imported `/experimental:c11atomics` option to MSVC's equivalent
`-experimental:c11atomics` spelling. sccache 0.18.0 treats the unrecognized slash
spelling as an input file, while retaining the dash spelling as an argument.
The SDK's C-only condition and atomics semantics are preserved; SDK files are
unchanged. Remove this normalization when the minimum supported Salts SDK
exports the corrected spelling.
CI resolves `Salts.Native` using `2.3.0-*` on every run and
builds the complete selected graph. The published package remains Runtime +
Component only; MIR remains outside the installed Runtime link interface.
Android uses the shared outer vcpkg toolchain with the NDK chainloaded, and its
existing installed consumers are cross-compiled without running host CTest.

Commit `27d6efb` passes all five profiles in both the
[cold build](https://github.com/qigao/turbowasm/actions/runs/37738464606) and
[cache-restored build](https://github.com/qigao/turbowasm/actions/runs/37738955984),
with zero cache write errors. Restored Linux and macOS MIR builds each record
300 hits and one miss and pass 209/209 tests. Windows records 196 hits, zero
misses and 76 uncached calls; its cacheable-hit percentage excludes those calls.
The same commit passes the four-platform
[SDK build and packaging](https://github.com/qigao/turbowasm/actions/runs/37738470020).

After atomics-option normalization, commit `2efaf00` passes the
[five-platform CI](https://github.com/qigao/turbowasm/actions/runs/37740255366)
and [four-platform SDK packaging](https://github.com/qigao/turbowasm/actions/runs/37740259898).
Its [restored Windows CI](https://github.com/qigao/turbowasm/actions/runs/37740607137/job/113190251265)
records 272 hits, zero misses, zero non-cacheable calls and zero cache write
errors, passing 170 tests plus 16 installed-package tests. The
[restored Windows SDK build](https://github.com/qigao/turbowasm/actions/runs/37740611588/job/113190297701)
records 56 hits and zero misses, non-cacheable calls or cache write errors.
The hit percentages exclude other compiler requests: sccache separately reports
two cache errors and eight non-cacheable compilations for Windows CI, and one
cache error and four non-cacheable compilations for the Windows SDK profile.

## SIMD

WebAssembly stores SIMD values as `v128`, but TurboWasm refines the semantic
lane shape after validation, for example:

```text
i32x4.add -> I32X4
f32x4.mul -> F32X4
i32x4.eq  -> B32X4
```

Storage stays a neutral 16-byte Salts carrier. Lane semantics map to canonical
CMeta descriptors, while execution uses `Salts::SIMD`.

The JIT keeps the same ownership model:

```text
validated SIMD
      |
      v
private invocation-local v128 slots
      |
      v
TurboWasm helper ABI
      |
      v
Salts::SIMD
```

The pinned MIR v1.0 backend does not provide a vector register type, so
TurboWasm does not claim a native MIR-v128 ABI. Helper-backed SIMD is the
canonical compiled path for this backend.

See [ARCHITECTURE.md](ARCHITECTURE.md) for the complete ownership and execution
model, and [docs/PORTABILITY.md](docs/PORTABILITY.md) for the C11, hosted-libc,
optional-service, and embedded build boundary.

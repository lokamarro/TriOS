# Optimization history

This is the repository's single history of attempted performance work. Entries
retain only the hypothesis, disposition, and durable lesson. They do not define
current behavior; current contracts live in architecture and subsystem docs.

`Accepted` means the useful mechanism remains or its result became product
policy. `Rejected` means the candidate was removed or disabled. `Measured`
means the work improved understanding without justifying a product change.

## General gadget forwarding

| Attempt | Disposition and lesson |
| --- | --- |
| Template-owned adjacent scalar result forwarding | Accepted as the default (`BuildOptions` can disable it per program for controls). Probes ran two to three times faster, full workloads only showed coverage; a mechanism control must match both code layout and data addresses. |
| Template-owned adjacent SIMD/FP result forwarding | Accepted beside scalar forwarding, with no new hot-path copies. Gains were small against shared-host variation, so coverage is not a broad speedup claim. |
| Combined scalar/SIMD dependency coverage | Measured. Broad contract coverage did not imply frequent adjacent dependencies; coverage counts execution-weighted opportunities, not speedup. The diagnostic was removed. |
| Preserve results across opposite-domain gadgets | Measured, not enabled; the added coverage was too little to justify separate preservation contracts. |

## Early gadget and MMU trials

| ID | Attempt | Disposition and lesson |
| --- | --- | --- |
| Trial 1 | Preserve gadget caches across address-space switches | Rejected; mixed results did not improve the aggregate. |
| Trial 2 | Share immutable gadget programs across vCPUs | Rejected; shared-miss synchronization overwhelmed short parallel work. |
| Trial 3 | Fixed set-associative cache with slab storage | Accepted; stable bounded storage removed full-cache clear cliffs. |
| Trial 4 | Data-only successor links | Accepted; cached blocks can link without executable publication. |
| Trial 5 | Specialized CMP/branch gadgets | Rejected after a marginal, workload-specific result. |
| Trial 6 | Reduce syscall-boundary state copying | Accepted; persistent per-vCPU state removed redundant frame work while preserving migration semantics. |
| Trial 7 | Batch cold fetches and pool translated programs | Accepted; cold process and translation workloads improved. |
| Trial 8 | Larger gadget cache and translation window | Rejected; capacity was not limiting and the larger window did extra work. |
| Trial 9 | Asynchronous Darwin UBD dispatch | Rejected as a performance change; correctness worked but cached benchmark work did not benefit. |
| Trial 10 | Keep guest NEON values in host registers | Rejected; the target workloads rarely used the selected regions. |
| Trial 11 | Vectorize bulk sparse-MMU translation | Rejected; sparse lookup and cached cross-page work dominated the vector setup. |

## Geekbench-guided gadget work

| ID | Attempt | Disposition and lesson |
| --- | --- | --- |
| G12 | One translation for same-page SIMD pairs | Accepted. |
| G13 | One translation for same-page integer pairs | Accepted. |
| G14 | Native host SIMD pair transfers | Accepted. |
| G15 | Native host integer pair transfers | Accepted. |
| G16 | Remove impossible scalar-byte page crossing | Accepted. |
| G17 | Direct ADD/SUB-immediate gadget addressing | Rejected; instruction reduction alone did not improve runtime. |
| G18 | Inline immutable direct-link records | Accepted. |
| G19 | Inline indirect-link cells | Accepted. |
| G20 | Pool link-map nodes | Rejected; added ownership cost without a useful gain. |
| G21 | Longer linked-chain checkpoint | Rejected. |
| G22 | Lightweight production builder callbacks | Rejected. |
| G23 | Combine scalar page and TLB checks | Rejected. |
| G24 | Predecode one-table SIMD lookup offsets | Accepted. |
| G25 | Predecode indexed floating-point vector offsets | Accepted. |
| G26 | Predecode basic vector floating-point offsets | Accepted. |
| G27 | Predecode three-same SIMD offsets | Accepted. |
| G28 | Predecode pairwise-long SIMD offsets | Rejected. |
| G29 | Predecode widening and high-narrow SIMD offsets | Accepted. |
| G30 | Dispatch-scoped native floating-point environment | Accepted. |
| G31 | Single link-map lookup | Accepted. |
| G32 | Bounded initial program reserve | Accepted. |
| G33/G33b/G33c | Cache active-link pointers | Rejected; lifetime and invalidation costs outweighed lookup savings. |
| G34 | Retain direct-link map nodes | Rejected for the same lifecycle pressure. |
| G35 | Branch-free logical MOV alias | Accepted. |
| G36 | Reuse ADD-immediate register-bank base | Rejected. |

The workload profile behind these trials showed that memory translation was
the broadest gadget cost, cold translation mattered most for compiler-style
work, and several image workloads were dominated by pair-vector transfers.

## Page size and native JIT campaign

| ID | Attempt | Disposition and lesson |
| --- | --- | --- |
| G37 | Make 16 KiB the default guest page profile | Accepted at the time, superseded by G281; multi-core gains outweighed a small single-core cost, with 4 KiB retained for compatibility. |
| G38-G41 | Smaller, one-way, wider, and folded 16 KiB TLBs | Rejected; direct-index conflicts or extra index work remained. |
| G42 | Two-way 16 KiB gadget TLB | Accepted; it removed deterministic alias conflicts while keeping a short primary hit path. |
| G43 | Strict native ARM64 JIT with host-safe feature exposure | Accepted. Pair-memory fast paths were separately rejected after instability or regression. |
| G44 | Reduce generated-code expansion and validate stale blocks exactly | Retained; block-local register reuse and exact saved-word validation became the durable direction. |
| G45 | Stateful native links and compact block boundaries | Accepted. |
| G46 | Deferred architectural state with one-to-one native shapes | Accepted. |
| G47 | Replace reservation serialization with a mutex | Rejected; convoy behavior worsened the gate. |
| G48 | Native remapping and cold translation cleanup | Accepted where code shape and semantics stayed exact. |
| G49 | Exact constants and constrained vector prebinding | Accepted. |
| G50 | Plan consecutive vector-list registers at translation time | Accepted. |
| G51 | Select resident GPRs from observed block use | Accepted. |
| G52 | Compare JIT and gadgets under matched Geekbench | Measured; the JIT won serial work but exposed a global publication bottleneck in several parallel workloads. |
| G53-G79 | Scale publication and recover gadget regressions | Accepted; ordinary stores became lock-free while uncommon reservation work retained synchronization. |
| G80-G84 | Shared templates and alternate flat JIT caches | Rejected; ownership and working-set costs were not justified. |
| G85 | Diagnose a JIT layout regression | Measured; large inline tables, not vCPU scheduling, caused the regression. |
| G86-G88 | Restore compact cache hashing and descriptors | Accepted. |
| G89 | Limit translated blocks to eight instructions | Rejected; short blocks sharply regressed compiler work. |
| G90-G92 | Heap-backed tables and construction-time verification selection | Accepted; large per-vCPU initialization and production polling were removed. |
| G93 | Always fetch the full translation window | Rejected; early termination remained valuable. |
| G94 | Replace direct opcode checks with a lookup table | Rejected by a matched control; direct checks stayed faster. |
| G95 | Final direct-check and desktop graphics baseline | Measured; confirmed the retained JIT state and removed the earlier scheduling concern. |

## 4 KiB page path

| ID | Attempt | Disposition and lesson |
| --- | --- | --- |
| G263 | Give the 4 KiB software TLB the 16 KiB geometry (two-way sets indexed by page number) | Rejected. Level on the Mac but a single-core loss on the iPad: likely cache footprint, one line per page against two pages per line in the folded table. |
| G264 | Fill a 4 KiB TLB miss's whole 16 KiB group from one static-MMU lookup | Rejected. Filling a whole 16 KiB group evicts live entries from the direct-mapped table; the 4 KiB miss stream is capacity and conflict, not cold. |
| G265 | Give the 4 KiB software TLB 16,384 one-way entries indexed directly by page number, with the page-crossing test moved to the miss path | Accepted. Direct page-number indexing with half the entries and a line shared by neighbouring pages brought 4 KiB to parity with 16 KiB where one line per page had lost. |
| G266 | Fault 4 KiB anonymous writes in aligned 16 KiB blocks, with minimum per-CPU page-list high marks | Rejected. Too small a gain to repay losing the sparse anonymous footprint Android guests depend on, a shared kernel topic that rebuilds every line, and a departure from upstream. |
| G269 | Publish a kernel write into guest code only once x86 code may run | Accepted. A 4 KiB uaccess write into a guest-code page reset the shared space's links on nearly every system call; only an x86 dispatch now publishes. Compare both page profiles' event counts, not only times. |
| G270 | Fill a gadget TLB miss through the executor's entered execution context | Accepted. The miss resolved the address space through `thread_local` (a `_tlv_get_addr()` call per access); on a Darwin hot path pass a resolved object down. |
| G271 | Attribute the 4 KiB Aquarium gap | Measured. The 4 KiB Aquarium gap split between gadget execution, revalidation after G269's publications, TLB fills and two 4 KiB-only kernel paths; G269 and G270 cut most of it. |
| G273 | Fill an entered context's TLB miss from a lock-free radix index of its generation's mappings | Accepted. A lock-free radix index of atomic words answers TLB fills (a 4 KiB guest refills twice as often); readers never see a stale word because every removal already quiesces execution. |
| G274 | Answer a 4 KiB uaccess backing sync under one lock | Accepted. One hold of the space's lock answers the 4 KiB uaccess backing sync; only a range past the direct slots asks the registry. |
| G275 | Read the Darwin carrier CPU from the thread's TSD instead of calling `pthread_getspecific()` | Accepted (kernel p4k1). The kernel keeps each carrier's CPU number in a second TSD key read at `TPIDRRO_EL0` behind a per-carrier check; kernel-owned state cannot answer (signal handlers run on the sigaltstack, XNU owns `TPIDR_EL0`). |
| G276 | Apply only changed `init_mm` PTEs in a kernel TLB sync | Accepted (p4k2). A kernel TLB sync applies only PTEs carrying `_PAGE_NEEDSYNC`; a vmalloc purge flushes the whole span between the areas it frees. |
| G277 | Invalidate arm64 translated code only over the changed range | Rejected before it was built: 4 KiB translates and validates no more than 16 KiB, and validation costs under half a percent of vCPU time, which bounds precise invalidation. |
| G278 | Plan no page for a batch unmap that names a page nothing holds | Accepted. An unmap plans only pages its generation or an earlier batch operation holds; the host planned every page of a TLB replay's unmap before finding it empty. |
| G279 | Send neighbouring Darwin user PTEs as one host call | Accepted (p4k4). Runs of absent PTEs, or PTEs mapping consecutive offsets of one backing with one protection, go as one host call; a dropped run is unobservable because an absent page faults back in. |
| G281 | Remove the 16 KiB guest page profile | Accepted, superseding G37. After G265 the 4 KiB profile matched 16 KiB, while keeping both cost a second TLB geometry, test matrix and runtime framework. The host page stays 16 KiB. Re-measure a default chosen to avoid a gap once its causes are fixed. |
| G285 | Make the fill index the static MMU's only per-page record, and drop the hash map beside it | Accepted. The mapping record fits in the radix word's spare bits, so the hash map beside G273's index went: a second index must hold something the first cannot. |

## Gadget-first and cache-budget campaign

| ID | Attempt | Disposition and lesson |
| --- | --- | --- |
| G96 | Define matched performance and code-layout controls | Accepted as campaign policy; later conclusions use focused controls rather than one noisy score. |
| G97 | Hash page addresses across four batch-planner buckets | Accepted. |
| G98 | Lower guest YIELD to host ISB in gadgets | Rejected; neutral aggregate and negative focused result. |
| G99 | Enable interprocedural optimization for gadget code | Accepted. |
| G100 | Compose five decode families in one translation unit | Accepted; this remains the audited `.ipp` exception. |
| G101 | Resolve the address space once per thread | Accepted. |
| G102 | Add ISB at JIT gate sites | Initial rejection withdrawn by G108 because the benchmark band was not established. |
| G103 | Resolve the per-thread execution object once | Accepted. |
| G104 | Use BFI/NEG for two gadget lowering patterns | Accepted on focused controls. |
| G105 | Measure host issue ratios | Measured; used to rank later changes. |
| G106 | Fold the vector-bank base into load displacement | Rejected; smaller code did not become faster. |
| G107 | Hoist successor fetch | Accepted for the bounded safe family; broader polymorphic claims were not made. |
| G108 | Establish the JIT noise band | Measured; retracted G102's unsupported rejection. |
| G109 | Validate stale blocks in bulk | Accepted. |
| G110 | Measure gadget-bank size headroom | Measured; prevented size assumptions from driving changes. |
| G111 | Fuse CMP-immediate with B.cond | Accepted on a focused probe. |
| G112 | Generalize fusion to destination-writing SUBS | Rejected. |
| G113 | Measure adjacent instruction pairs | Measured; used to choose later fusions. |
| G114 | Fold ADRP plus ADD | Accepted. |
| G115 | Remove NOP entries | Not implemented; the evidence contract did not justify it. |
| G116 | Profile instruction pairs over a full workload | Measured; corrected the NOP and conditional-pair priorities. |
| G117 | Fuse register CMP with B.cond | Accepted; further fusion had a small ceiling. |
| G118 | Replace the MMU miss structure with a radix table | Rejected before implementation after measurement disproved the premise. |
| G119 | Remove the mapping lock from the miss path | Rejected; it produced no useful change. |
| G120 | Isolate lifecycle-lease cost | Measured; identified the actual miss-path target. |
| G121 | Replace the lease with a published per-thread access slot | Accepted. |
| G122 | Use a FIFO ticket turn | Rejected as neutral. |
| G123 | Serialize Darwin kernel mappings and repair whole host slots | Accepted as a correctness repair; no performance claim. |
| G124 | Establish a post-migration baseline | Measured; an earlier PSI conclusion was withdrawn. |
| G125 | Expose the host-safe guest feature surface | Accepted; vector workloads had been feature-starved. |
| G126 | Requalify the feature surface end to end | Accepted. |
| G127 | Investigate Text Processing scaling | Measured; the workload itself exposes limited parallelism, so no emulator change followed. |
| G128 | Bound the possible value of a fixed vector bank | Measured in the JIT. |
| G129 | Try gadget-shaped fixed-bank routes | One route was rejected and one required better instrumentation. |
| G130 | Measure a fixed gadget vector bank | Measured; no prototype was kept. |
| G131 | Profile the JIT store gate | Measured; score-only evidence could not settle the ISB idea. |
| G132 | Make the publication store scan lock-free | Accepted. |
| G133 | Greatly enlarge JIT cache and arena | Superseded; it fixed thrash but used an unbounded per-executor shape. |
| G134 | Share JIT translations between executors | Retained for bounded reuse and ownership; the measured throughput result was neutral to negative. |
| G135 | Derive JIT cache geometry from one process budget | Accepted; corrected G133. |
| G136 | Bound shared storage, reporting, and executor-pool state | Cache and reporting foundations retained; executor-pool performance A/Bs were rejected. |
| G137 | Carry guest x8/x9 across JIT links | Rejected; a focused gain did not become a broad workload gain, and the carry was later removed (see "Enable linked x8/x9 carry" below). |
| G138 | Cap private JIT state and use compact link anchors | Accepted. |
| G139 | Sweep smaller private-cache ceilings | Rejected below the retained 64 MiB ceiling. |

## iOS, graphics, and later gadget work

| ID | Attempt | Disposition and lesson |
| --- | --- | --- |
| G140 | Keep a bounded SIMD destination resident in gadgets | Accepted only for the proven nonfaulting family. |
| G141 | Reuse stale gadget programs after exact word validation | Accepted. |
| G142 | Share decoded gadget templates | Closed before implementation; the expected gain did not justify executable/code complexity. |
| G143 | Profile gadget-engine time by category | Measured; guided the next candidate set. |
| G144 | Ten post-profile gadget candidates | Rejected; none beat controls without unacceptable cost. |
| G145 | Requalify common iOS-style memory and persistent carriers | Accepted architecture; a separate SDOT residency arm remained rejected. |
| G146 | Requalify the 4 KiB high-address profile | Measured; no product change. |
| G147 | Profile completed workloads on physical iPad | Measured; host service rate, not missing carriers, dominated. |
| G148 | Compare page profiles with four vCPUs | Measured; no product change. |
| G149 | Repeat slow iPad workloads | Measured; confirmed device-specific throughput limits. |
| G150 | Check a 1.5 GiB physical-iPad workload subset | Measured as a fit result only; it did not establish desktop capacity or a speedup. |
| G151 | Compare macOS and iPad regression traces | Measured; no product change. |
| G152 | Compare equal guest instruction throughput across hosts | Measured; rejected a simple instruction-latency explanation. |
| G153 | Run equal-work vector-store probes | Measured; rejected the HDR vector-store hypothesis. |
| G154 | Vary vCPU count for iOS KDE bring-up | Measured; startup was limited by available guest CPUs rather than translation. |
| G155 | Move the KDE text getty off the compositor VT | Accepted as a narrow launcher compatibility fix. |
| G156 | Compare WebGL Aquarium at matched resolution | Measured; Simulator presentation was slower, with no immediate product change. |
| G157 | Try JIT for Simulator Aquarium | Rejected; it was slower and did not address the dominant wait. |
| G158 | Memoize gadget link records during reactivation | Accepted. |
| G159 | Reset only activated gadget links | Rejected; the removed work was too small to matter. |
| G160 | Keep several tagged address spaces in one gadget cache | Accepted; whole-cache switching was removed. |
| G161 | Share gadget translations across executors | Rejected: adopting tiny programs cost more than rebuilding them. The mechanism stayed off and was removed after G166. |
| G162 | Inline a conditional branch's fall-through | Accepted and materially beneficial; broader noncontiguous continuation was rejected. |
| G163 | Inline one direct-call callee | Not demonstrated, and removed. Inlining one direct-call callee per site was within noise and at every site an exact wash, as duplication cancelled the removed boundaries. |
| G164 | Recheck cache and block-length work on Simulator | Measured; confirmed the retained direction. |
| G165 | Profile Simulator Aquarium cache and CPU time | Measured; boot was cache/CPU-sensitive while steady frames were host-round-trip-limited. |
| G166 | Add or fund a 512 MiB shared gadget tier | Rejected in both forms, tier removed. The translator is only a few percent of a vCPU's time, so an extra tier swaps build time for store time or loses cheap tagged hits. |
| G167 | Give each address space owned cache state | Accepted; retirement became proportional to owned entries. |
| G168 | Recheck G167 on Simulator | Measured; confirmed startup benefits without a steady-frame claim. |
| G169 | Cache the Darwin current-vCPU lookup | Accepted on the hot-path profile, not as a frame-rate claim. |
| G170 | Decouple guest completion from host presentation | Retained as a correctness/latency boundary; it was performance-neutral and retracted an occlusion-biased native-display result. |
| G171 | Attribute the Simulator/macOS Aquarium gap | Measured; synchronous Simulator framework round trips dominated, and the obvious batching change was rejected. |
| G172 | Reduce UI-thread presentation work | Claim-before-drawable ordering accepted; fallback-buffer rotation and a separate presenter thread were not justified. |
| G173 | Bulk-own gadget generation allocations | Accepted; removed allocator-heavy Android boot stalls. |
| G174 | Rebind link state when a cache slot changes owner | Accepted correctness fix. |
| G175 | Profile Android boot carriers and dependencies | Measured; ART/dependency work and A15 host service dominated, not missing carriers. |
| G176 | Prototype asymmetric virtual CPUs | Feasible, but exact Apple core binding was unavailable through supported APIs. |
| G177 | Qualify asymmetric topology | Rejected; it reduced useful scheduling capacity and regressed boot. Uniform vCPUs remain current. |
| G178 | Measure a no-exit gadget ceiling | Measured. A dependent ADD-immediate stream with no executor exits sustained about 15 percent of native throughput: a best-case scalar ceiling, not a whole-engine ratio. |
| G179 | Hoist native FP-environment activation to program entry | Rejected. Removing the native-FP guard shrank the bank but measured zero mean: guard work overlaps the dispatch dependency chain, so bank shrinkage alone establishes no gain. |
| G180 | Bias linked-chain retirement accounting | Accepted. A signed counter bias replaced each linked edge's compare-and-branch with one sign-bit test and shrank gadget text. |
| G181 | Add a generation-shared target-keyed tier after the two-way indirect-site cache | Rejected. A shared target-keyed tier sped a polymorphic probe but cost general integer workloads 4-7% single-core: an extra lookup on residual misses is too broad. |
| G182 | Retire only links whose translated code overlaps a sustained code-change range | Rejected. Selective link retirement sped invalidation probes but lost 6-8% in four-vCPU Aquarium; whole-reset stays. |
| G183 | Outline repeated TLB-miss helper tails | Rejected. Sharing cold TLB-miss tails cut text 5% at zero mean: bank shrinkage alone does not pay for another call and return on misses. |
| G184 | Schedule conditional-branch NZCV restoration before independent stream loads | Rejected. Scheduling the NZCV restoration before independent stream loads measured within noise; the system-register write exposed no useful overlap. |
| G185 | Forward native NZCV into an adjacent conditional branch through shared interior entries | Rejected. Skipping a system-register write by forwarding native NZCV measured within noise; dispatch and stream-load latency hid it even in the most favorable loop. |
| G186 | Keep the scalar top-byte address mask in one shared callee-saved register | Rejected. Computing the top-byte mask once removed a hot instruction per transfer but measured slightly slower: the removed work overlaps the TLB dependency chain. |
| G187 | Put the scalar software-TLB way-zero hit in fall-through order | Rejected. The layout-only transform measured with opposite signs; the predicted taken branch is not a throughput limiter. |
| G188 | Hoist scalar-memory successor fetch across address translation | Rejected. The hoist cut a hot-memory probe's time but four-vCPU Aquarium lost several percent: a hot-memory latency gain did not generalize. |
| G189 | Mirror the retained arm64 cache hierarchy in x86 gadgets | Accepted after desktop-boot profiling: bounded four-way program storage, a two-way successor tier, and direct and indirect links, without weakening exact source validation. |
| G190 | Bulk-own x86 cache-generation allocations and reuse validation buffers | Rejected; matched desktop boots were neutral to slightly slower. |
| G191 | Inline exact validation for tiny x86 source spans | Accepted; most profiled validations were at most 32 bytes, so inlining exact validation for them shortened a full smoke boot by about five percent. |
| G192 | Measure realistic scalar and SIMD gadget ceilings | Measured. Realistic scalar and FP loops ran far below the dependent ADD chain's ceiling, at about four host cycles per scalar word. |
| G193 | Attribute the realistic ceiling cost with hardware counters | Measured. Above the dispatch floor the mixes are bound by dependency latency and misprediction refill, not instruction count; a reduction pays only on the critical path. |
| G194 | Keep write-through results in a bank of forwarding slots instead of one adjacent register | Rejected at one and two slots. Forwarding slots that outlive intervening entries did not help: loop-carried values cross block links, where every slot dies. |
| G195 | Reorder a block's instructions before translation so consumers follow producers | Rejected, removed. A list scheduler slowed everything: the host core already overlaps independent chains, so hop count, not ordering, is the lever for a data-only engine. |
| G196 | Address an in-place gadget's CpuState slot by a baked displacement instead of a decoded register index | Accepted with G199 and G200. A CpuState store-to-load pair forwards at rename only with a base-plus-displacement address, so baking the displacement beats a decoded register index; affordable only in the in-place shape. |
| G197 | Give a template byte-identical copies so independent guest registers do not share one load and store program counter | Rejected, copies removed. Byte-identical copies sped dense probes, but duplicated entry text is charged to every workload. |
| G198 | Repeat the in-place displacement measurement on a second core generation | Measured on an A15. The in-place displacement gain transfers across core generations; trust the interleaved-chain probe across cores. |
| G199 | Bake the transferred register of a scalar memory transfer as a CpuState displacement, extending the in-place shape from arithmetic to memory | Accepted with G200. Baking the transferred register gains more than baking the base; a CpuState hop needs the same addressing form at both ends or it runs slower than unbaked. |
| G200 | Keep a baked family's cold tails out of its hot path so the copies cost only what differs | Accepted; makes G196 and G199 default-on. Moving a baked family's cold tails out of the hot path halved its bank growth: measure a family by hot bytes, not total. |
| G201 | Store a gadget translation as one exact-sized allocation instead of a pooled body with a doubling stream | Accepted. One exact-sized allocation per program shrank caches and peak RSS; a translation cache's allocator is part of its footprint. |
| G202 | Share gadget program bodies between executors running one address space | Rejected before implementation: G201's census found little duplication between executors (G226 later showed multithreaded guests differ). |
| G203 | Replace the stream-address index with the body arena's own map of where live translations start | Accepted. The body arena's own start bitmap answers what a per-store stream-address index answered; when the writer is the allocator, ask the allocator. |
| G204 | Replace the per-generation mappings `std::shared_mutex` with a word-sized reader-writer spin lock, and take the two process-wide locks off the miss path | Accepted. A libc++ `shared_mutex` around per-generation mappings capped 4 KiB multi-core scaling; a word-sized reader-writer spin lock fixed it. G119's rejection was about one bottleneck order, not the lock. |
| G205 | Answer "does any native shadow alias this backing page" from a lock-free filter before the shadow index | Accepted. A lock-free hashed filter answers whether a native shadow aliases a page, instead of an `fstat` and a process-wide mutex per mapped page. |
| G206 | Put the word-sized reader-writer lock on the three remaining libc++ locks a vCPU takes around every guest entry and fault | Accepted. The remaining libc++ locks on every guest entry and fault became G204's lock as `WordSharedMutex`, sleeping after a long spin since a lifecycle writer can wait out a vCPU. |
| G207 | Let the threads of one process replay their page-table changes side by side: no per-mm turnstile, exact user ranges, and no replay in front of a fault that Linux has to resolve anyway | Accepted, both kernel lines. Dropping the per-mm turnstile and the replay in front of faults Linux resolves anyway raised first-touch throughput; replays need bounded batches, since one batch livelocked an Android boot. |
| G208 | Size the private gadget cache from the working set it has to hold, and let the product decide what it can afford | Accepted, with x86's X58: multi-core Clang thrashed the private cache through way replacement alone (slot counts were later settled by G280). G166's downward sweep found the cliff, misread as the knee. |
| G209 | Choose the victim way by second chance instead of round robin | Rejected, with x86's X59. Second-chance victim selection was a wash: a policy learns only from lookups, and the hottest blocks are reached through links that never look up. |
| G210 | Keep an address space's link records and indirect sites in chunks behind one open-addressed index instead of pooled node maps | Accepted. Chunked link records and one open-addressed index shrank link state severalfold; the allocator of records read on every block transition shapes the dispatcher's footprint. |
| G211 | Keep only what a stored arm64 block cannot recompute in its body header | Accepted. A stored block's header carried the builder's whole `BuildResult`; it keeps only what the block cannot recompute. |
| G212 | Name a stream's gadgets by 32-bit offset instead of by pointer | Measured, with x86's X76; not built. A 32-bit gadget offset adds a dependent instruction to every dispatch and cost a few percent: price any dispatch change against that. |
| G213 | Keep an arm64 link record to its BlockLink and one exit word | Accepted. A link record keeps its BlockLink and one exit word; state only some targets need belongs with those targets. |
| G214 | Name a record-table entry by its record's number instead of its address | Accepted. A record is named by the number it was carved under instead of by pointer. Isolate a one-sided multi-core reading led by a noisy workload before believing it. |
| G215 | Report an unsigned-offset or pair transfer's fault by its position in the block | Accepted. A guest-PC word only a fault reads is recovered from the block position carried in spare descriptor bits; the fault path can afford to recompute what it alone reads. |
| G216 | Read a linked call's return address out of its return link | Accepted. A linked call no longer carries a return address that the return link beside it already holds. |
| G217 | Gather the hottest gadget entries into one page-aligned section | Rejected, with x86's X83. Gathering hot entries cut L1I misses but left IPC level: fetch restarts at indirect dispatch dwarf them. |
| G218 | Shape a stored arm64 translation for the data cache: its stream apart from its validation words, streams on 64-byte boundaries, and a linked successor's stream prefetched 64 bytes ahead | Rejected, all three (stream/validation split, 64-byte alignment, successor prefetch): the out-of-order core hides these predictable loads; measure elasticity first. |
| G219 | Resolve a vCPU thread's address space without a shared reference count | Accepted. The thread-local address-space cache lends its entry instead of handing back an owning reference whose count every vCPU shares. |
| G220 | Follow indirect-site misses through a per-address-space target table | Accepted. A direct-mapped {target, block link} table per address space (a ring for returns) cut executor round trips at many-target indirect sites about threefold; why it beat G181's larger tier was not isolated. |
| G221 | Strip a top-byte-ignore tag in line on the vector and SIMD structure transfers | Accepted, but its premise was wrong: bionic stayed at heap tagging NONE, so the tags came from Scudo headers and other non-heap pointers. Vector, pair and structure forms strip the top byte in line; G284 priced the rest. |
| G222 | Flush a vCPU's software TLB by the sets it filled, and invalidate a range wider than the table by key | Accepted. A TLB flush clears only the sets fills recorded; an invalidation wider than the table walks entries by key. |
| G223 | Make a linked call and its return cheaper | Rejected before implementation. Returns already stay in the stream and removing the return-record push outright made no probe faster. |
| G224 | Fuse the hottest adjacent gadget pairs of loader-like code | Not pursued. Pair frequencies were flat and led by independent operations the core already overlaps; the best case bounds the gain near 2 percent, as G117 found. |
| G225 | Stop fewer vCPUs for a guest unmap | Measured; no change beyond G222. The wait for running vCPUs on a guest unmap is a small share even of an unmap-churn probe. |
| G226 | Census where the gadget caches' bytes go and why they miss before choosing a memory cut | Measured; no engine change. Multithreaded Geekbench left two fifths of a cache in bodies another executor of the same address space also held. Census a cache while its workload runs and count duplication by sharing executors. |
| G227 | Share an address space's translations among the executors running it | Accepted; off at first, default after G267 and G268 (`DARWIN_UML_GADGET_SHARED_BODIES=0` turns it off). Arm64 executors of one address space share bodies, links, sites and target table, halving multi-core cache; hit-path writes must be write-once first. x86 stays private. |
| G228 | Drop the guest-PC word from the remaining arm64 memory transfers | Accepted. The remaining arm64 transfer forms keep the block position in the operand's top byte (a literal its target) instead of a stream word only faults read. |
| G229 | Keep more address spaces resident in a gadget cache: a window of twelve, sixteen or twenty-four instead of eight | Rejected. A larger resident-address-space window raised hit rate only by growing the cache severalfold: residency costs the state each resident space keeps, not the slots its code occupies. |
| G230 | Give a way a lookup found a second chance before its set replaces it | Rejected, with x86's X86. A second-chance mark raised hit rate a tenth of a point for a read-modify-write per hit; the remaining misses come from sets too small for their blocks. |
| G231 | Give each gadget cache set eight ways instead of four | Kept, with x86's X87. Eight ways per set raised multi-core Geekbench and Aquarium at 131,072 slots for little memory; a wider set keeps what a narrow one evicts. |
| G232 | Give macOS and iOS one gadget-cache slot count | Accepted as the user's interim default of 131,072 slots on both products and ISAs until G280. A CMake cache entry holding a product default reaches only new trees unless it can tell a value nobody chose from one somebody did. |
| G233 | Pack an arm64 gadget cache slot into three words | Accepted. Every slot byte is paid by every way of every executor; a slot is three words (tag with a 16-bit address-space id, translation, memo with a 16-bit epoch). Compare what only validation or a link needs once per address-space change, not per way. |
| G234 | Halve an arm64 translation's header from 32 to 16 bytes | Accepted, with G233. A 16-byte header keeps what a block cannot recompute; where a build stopped lives in a tail only some blocks carry. |
| G235 | Keep what a gadget cache's ways evict in a packed victim tier | Rejected, with x86's X90. A packed victim tier matched twice the slots' hit rate in less memory but cost more vCPU time than the translation saved; sample how much of a miss is translation (G166) before building one. |
| G236 | Store a MOVZ or MOVN constant in one gadget stream word | Accepted. A MOVZ/MOVN constant of at most 48 bits carries the destination's CpuState offset in its word. Timing of G236 to G239 is under G239. |
| G237 | Store an ADR or ADRP address, and an emulated identification register, in the same word | Accepted, after G236. ADR/ADRP (with a folded ADD) and MRS identification-register reads use G236's entries. |
| G238 | Carry a TBZ or TBNZ's bit number in its operand word | Accepted, after G237. A test-bit branch's bit number sits in the operand word; the old entries stay in the bank unselected, since removing them would move every later entry. |
| G239 | Pack a 32-bit logical immediate into the register word | Accepted, after G238. A 32-bit logical immediate shares the register word. Stream words are bytes, not executed instructions: four packings saved cache memory and moved no timing. |
| G240 | Run SBFM and UBFM as one or two register shifts instead of masks | Accepted. SBFM/UBFM are a left then a right shift with amounts the encoding fixes: when the architecture fixes an operand at translation, the builder evaluates it and the gadget applies it. |
| G241 | Bake X30 into the entry of a RET through it | Accepted, after G240. Every RET in the benchmark binary returns through X30, so the entry holds it at a fixed displacement; an operand the same in nearly every translation belongs in the gadget. |
| G256 | Release the link records and indirect sites nothing names any more | Accepted, with x86's X94. A private space sweeps link records and sites nothing names once they number half again what the last sweep kept; a shared space (G227) is not swept. |
| G257 | Reuse freed arm64 body-arena space by best fit, and map its chunks in regions | Accepted. Heap-backed arena chunks kept freed blocks in the footprint and a freed block served only its own size; arenas now best-fit and map their own 64 KiB chunks. Give freed memory back to the system, not to a heap. |
| G258 | Keep emptied arm64 arena chunks in a process-wide set for the next arena | Rejected. A process-wide reserve of emptied chunks cut faults but left Aquarium level: the load-time cost was per-chunk arena work, which G257 removed. Price a mapping in the workload before spending footprint to avoid it. |
| G262 | Make the two ends of a CpuState hop around an in-place entry agree in addressing form | Rejected. Mixed baked-and-indexed hops are mostly a baked writer read by an indexed reader, which costs exactly an indexed hop; adapters cost 2 percent. A hop-form fix pays only where the hop is critical. |
| G267 | Replace a shared gadget space whose arena is mostly freed | Accepted. Under churn a shared space kept link records and freed arena that private copies shed; once at least 2 MiB are unused and more than its bodies use, it is closed and replaced. Bound a lifetime a sweep cannot reach by replacing the container. |
| G268 | Share an address space's translations among its executors by default | Accepted. With G267 bounding it, sharing was level with private on three hosts, so a host with FEAT_LSE2 (every Apple Silicon product) shares unless `DARWIN_UML_GADGET_SHARED_BODIES=0`. |
| G272 | Let go of the gadget spaces of address spaces the kernel destroyed | Accepted. An executor's window kept destroyed address spaces; it now lets go of them, leaving a tombstone so live spaces are displaced exactly when they were. |
| G280 | Settle the gadget-cache slot count at 262,144 on both products and both ISAs | Accepted, ending G232's interim: 262,144 slots on both products and ISAs. Settle a capacity where measured workloads stop evicting, and check with a layout control that a size-changing build's gain is its own. |
| G282 | Bound the bytes the gadget caches keep with a per-process budget | Accepted. Both engines charge each address space's bytes to one process-wide budget (a quarter of guest memory, at least 128 MiB, at most an eighth of the host's process limit); a store past it sheds from the largest. Sized never to act on measured workloads. |
| G283 | Index the arm64 gadget cache sets by a multiply | Measured; not built. The arm64 set fold was within 7% of a uniform spread (x86's was ten times it): change an index only where resident sets load unevenly. |
| G284 | Strip a top-byte-ignore tag in line on the atomic, ordered, RCpc and exclusive transfers | Accepted. Atomic, ordered, RCpc and exclusive gadgets keyed the TLB by the tagged address, so tagged and plain pointers to one page evicted each other; one `ubfx` keys by the untagged page. Time a form against a plain access of the same page. |
| G286 | Give each guest PC one of several copies of the A64 gadget bank so that its dispatch trains its own predictor entries | Rejected; prototype dropped. Four hashed copies of the A64 base split indirect-predictor state but raised mispredicts and I-cache misses and cut IPC about 20 percent: the predictor lacks capacity, not context, so only fewer dispatches help. |
| G287 | Hold an arm64 gadget-cache slot in two words | Accepted. A slot's link-record memo cost 2 MiB per vCPU whatever the guest ran; the record is found through the per-cache memo indirect misses already use. State kept per way is multiplied by every way of every executor. |
| G288 | Carve arm64 gadget link records 32 bytes apart | Accepted. A link record keeps only what generated code reads (BlockLink and the `link_exit` word); what only the cache reads sits in a companion behind the chunk's records. Split a record by reader. |
| G289 | Speed up the COLRv1 emoji painting of a UI rendering CTS test on the gadget engine | Measured; nothing to build. The COLRv1 emoji render is scalar Skia, FreeType and scudo code at about half the ideal gadget rate with no pathology; replicating the bank (G286) made it worse and the remaining levers total 5 to 8 percent. The product accepts the iPad's margin; Mac failures under load are attributed to load. |

## Host instructions per guest instruction

| Attempt | Disposition and lesson |
| --- | --- |
| Measure host instructions per guest instruction | Measured. Almost all host instructions are in gadget entries, memory transfers are the largest share of fast-path cost, and a few hundred hot entries cover nearly every execution. |
| Count why linked chains return to the executor | Measured. Most round trips are indirect-site misses, mostly returns at sites with more targets than a small cell array holds; this motivated the return-record ring and G220. |
| Link the exit of a block cut short by the fetch window | Accepted. A straight run of more than 64 words is several blocks and each exit returned to the executor; linking the exit as a branch does made an unrolled dgemm and HPL much faster. |
| Return-record ring for cached returns | Accepted, unconditional. Linked calls push {return PC, BlockLink} on a private sixteen-record ring that a missed return pops, removing most return misses; the ring must train the cell as the cold helper would. |
| Tagged TLB keys | Rejected, switch removed. Keying by the address as presented drops the top-byte check but measured slower: the check overlaps the TLB dependency chain (as in G186, G187), and MTE guests would key several tags of one page into one set. |
| Fused transfer translation | Accepted, unconditional. Keying same-page transfers by the final byte's page while indexing by the first folds the cross-page check into the tag compare. A shifted-operand `cmp` was slower than a separate shift and compare. |
| Four indirect cells and NOP elision | Four cells: removed; they cover only the minority of misses. NOP elision: rejected, switch removed; a dispatched NOP retires in the shadow of the gadgets around it. |

## Android boot and memory investigations

| Attempt | Disposition and lesson |
| --- | --- |
| Matched cold/warm platform timing | Early Android startup was not the bottleneck; first-boot ART and package verification dominated. |
| macOS vCPU sweep at 4 GiB | Measured. Eight uniform vCPUs were the boot-time sweet spot on the M1 Ultra host; ten gave no benefit. |
| Physical-iPad vCPU sweep at 1792 MiB | Measured. Four uniform vCPUs were the boot-time sweet spot on the 4 GiB iPad; six was not viable, as iPadOS killed every cold attempt under memory pressure. |
| Build-time ART placement | Accepted. Ordinary application artifacts are installed beside their APKs instead of being routed to an unbuilt `system_other` image. APEX-contained apps remain a separate platform boundary. |
| ext4 Android payloads and APEXes | Superseded by compressed EROFS: read-only partitions and APEX payloads are Zstandard level 15 with 16 KiB clusters, and `.apex` files stay uncompressed so apexd's loop devices keep direct I/O. Cold launches equal ext4 at 4 GiB and run longer at 1,536 MiB. |
| Anonymous host VM for Android RAM | Accepted. It removed file-pager churn from cold boot while preserving ordinary file-backed disk behavior. |
| Full physical-device RAM wiring | Superseded by on-demand commitment. Wiring all of `mem=` at startup charged the whole ceiling to the footprint whether or not the guest used it; physical iOS wires a guest-RAM page only while the guest holds it. |
| On-demand guest RAM with page-cache trim | Accepted. Committing guest RAM on demand and trimming the inactive page cache about halved the iPad's footprint peak, at the cost of slower cold allocation and rereads. On iOS, wiring chunks of untouched memory gives each a private VM object `MADV_FREE_REUSABLE` never frees, so the RAM root must be one object first. |
| Kernel physical window as the Android RAM root | Accepted. The software MMU and host services borrow the kernel's linear window while protection-specific native mappings remain shared aliases; the separate full-size root mapping disappeared. |
| Drop the image's relocation records, and allocate the shared-memory tables on demand | Rejected. Relocation records stay because pages freed below the break lie outside the vhost-user memory table; the tables stay static because the map request runs in interrupt context, where an atomic allocation can fail. |
| Apple asymmetric CPU topology | Rejected and reverted. Public topology/QoS hints could steer carriers but reduced useful Android capacity on the target device. |
| Physical-device boot profiling | Measured. After RAM changes, the remaining gap is dominated by host execution and scheduling service for the full gadget/MMU working set. |
| Expedited RCU grace periods until boot completes | Accepted. Mainline ends the boot-time expedite when init starts, where the Android common kernel kept it for 20 s; expediting through `sys.boot_completed` reached boot completion about 2.7 s earlier. Normal grace periods return after boot. |
| Batch sparse UBD discard | Accepted in both maintained kernel lines. A 512 MiB raw discard collapsed from 16,384 requests to four, an order of magnitude lower wall time on macOS and the iPad while reclaiming the same allocation. |
| Engine metadata in domain heaps and thread scratch | Accepted. A restarted guest left the MMU's indexes in the default malloc zone, which keeps emptied chunks and flips their pages in the footprint; they now live in per-domain heaps released whole. A footprint that flips cannot be bounded from the zone's side: the owner owns the lifetime. |
| Gadget program caches and record tables in domain heaps | Accepted, because the Geekbench gate found it neutral or faster. Slabs come from a heap per executor; address spaces share one record-table heap that goes back with the last lease, the lease count and release in one critical section. |
| Mesa and the GPU stack's allocations in a slot heap | Rejected; removed. A slot heap removed the footprint flips on macOS, but on the iPad its window filled, requests fell back to libc and footprint and CPU ended higher. iOS already runs `MallocSpaceEfficient=1`: measure a footprint remedy on the shipping platform's allocator; an optimization that helps only macOS stays out. |
| Lower the Android decoder's output delay to the stream's reorder depth | Rejected; not merged. An ABBA found the player's dma-bufs byte-identical, and a device stating no depth would stall deeper streams. Count the guest's dma-bufs before crediting a framework parameter with a saving. |
| Spend the Android decoder's stall limit on input blocks, not capture buffers | Accepted. The framework sizes the capture queue from the output delay alone and the input queue from the pipeline delay alone, so output delay 4 with pipeline delay 9 keeps the stall limit with 8 capture buffers instead of 14; a busy 2 vCPU iPad dropped frames with 8, so 1080p or less keeps 12. |

## Android application launch

| Attempt | Disposition and lesson |
| --- | --- |
| Keep the native display out of App Nap | Accepted. Bimodal launch times on the macOS native display were App Nap: an occluded window or locked screen put every emulator thread in the background band. The display holds a latency-critical activity; record thread priorities with any host timing. |
| Look for a launch regression in recent emulator, GPU, and release changes | Measured; none found. The apparent regression was App Nap stretching the platform work around Unity's own phases. |
| One flat catalog index for the builder's per-instruction contract probe | Measured, not adopted. One open-addressed table made the probe a tenth faster, but translation is a small share of launch CPU, so the gain was inside launch noise. |
| Measure general engine work on application launches | Measured. G219 and G220 shortened Angry Birds 2 and 3DMark launches by a couple of percent; engine fixed costs move a launch only in proportion to their share of it. |
| Profile 3DMark Wild Life Extreme's loading phase | Measured. The phase is one guest thread of scalar loading code with no I/O wait and an idle GPU, so it moves only with per-instruction cost (G221 to G225). It is bimodal per boot: compare boots in the same mode. |

## JIT multicore and browser working sets

| Attempt | Disposition and lesson |
| --- | --- |
| Isolate JIT store-publication state at the host cache-line size | Accepted. Adjacent 64-byte vCPU slots shared an Apple arm64 cache line; 128-byte slots and separated gate fields improved four-worker throughput. |
| Replace unused shared JIT translations within the process budget | Accepted. Bounded replacement keeps startup code from permanently crowding out later working sets. |
| Fund larger private JIT working sets within the process budget | Accepted. A 256 MiB private ceiling reduces browser startup cache resets while the total stays tied to guest RAM; smaller budgets shrink private geometry first. |
| Retain address-space link activation in writable data | Accepted. Generation-tagged direct and indirect links retain trained targets across unchanged address-space switches, and link updates no longer open executable write windows. |
| Lower bounded compare/exchange retry regions natively | Accepted. Native exclusives preserve width, ordering, status, flags and retired prefixes; read-only mismatches and tagged accesses keep their semantic behavior. |
| Remove speculative GPR residency recompilation from runtime misses | Accepted. Cold translation profiles exposed a second lowering pass on runtime misses; one pass removes the duplicate work. |
| Admit repeated ordinary blocks to native caches | Rejected; the option was removed with the JIT's interpreter fallback. Admission regressed single-core File Compression with little multicore benefit (an earlier attribution was withdrawn: a PAuth gate left admission inactive). |
| Reuse peer translations before cold admission | Removed with the admission option. An executor uses an exactly validated shared translation on its first local visit instead of repeating semantic execution. |
| Release the MMU execution reader during native compilation | Rejected. No meaningful four-worker gain; cold compilation was not the remaining shared-MM bottleneck. |
| Plan single-page MMU batches without temporary containers | Rejected. The initial gain did not reproduce; container allocation was not the dominant cost of the shared-MM waits. |
| Remove redundant kernel fault mapping walks | Rejected. Gains were inconsistent; the existing fault repair stays until a repeatable improvement justifies the shared kernel change. |
| Enable generic kernel optimistic spinning | Rejected. Mixed multicore results and no repeatable scaling gain; the existing lock policy stays. |
| Drain IPI pipes using completed-write counts | Rejected. Removing poll calls gave no repeatable four-worker gain; the existing drain protocol stays. |
| Isolate kernel carrier flags and counters on 128-byte host cache lines | Rejected. Aggregate multicore throughput fell; packed carrier state was not the remaining scaling bottleneck. |
| Publish data permission increases without stopping execution | Rejected. Mixed results and no meaningful scaling gain; the general mapping barrier is not weakened without a workload benefit. |
| Reuse pinned state for TLB history reads | Rejected. No meaningful gain; registry contention visible in samples did not translate into throughput. |
| Skip translated-cache reconciliation for first page publication | Rejected. No repeatable gain; the existing translation invalidation policy stays. |
| Use an exclusive mutex for address-space mapping metadata | Rejected. Short trials suggested a small gain, but the full comparison left scaling unchanged and reversed one result; concurrent metadata readers stay. |
| Defer contended MMU replay at ordinary userspace returns | Rejected. A small inconsistent gain while traces kept the fault-path mutex contention: forced fault repair remained the dominant source of waits. |
| Replace MMU shared mutexes with native pthread reader/writer locks | Rejected before product changes. An isolated comparison improved uncontended access but substantially reduced four-thread read-only and mixed throughput. |
| Avoid code-version changes for first page publication | Rejected. No throughput gain; conservative code publication semantics stay. |
| Require a third visit before native admission | Rejected. Production admission was inactive during the timing trials, so the scores do not justify changing the threshold; the admission option was later removed. |
| Increase JIT dispatches from 64K to 256K guest instructions | Rejected. No multicore gain; the kernel-entry budget stays. |
| Increase native JIT chains from 4K to 16K guest instructions | Rejected. A small multicore gain with single-core variation; the 4K fairness checkpoint stays until an isolated comparison justifies longer preemption intervals. |
| Reuse an already-owned FP environment from scalar chain entries | Rejected. A small HDR gain but slower Photo Filter in both runs; broader chaining did not improve multicore scaling. |
| Publish store eligibility from read-only JIT chain entries | Rejected. No reliable scaling gain, and broader chains need explicit handling of a successor's deferred reservation exit. |
| Shorten JIT dispatches from 64K to 4K guest instructions | Rejected. Chromium still missed its ordinary startup deadline and multicore throughput fell. |
| Enable linked x8/x9 carry or isolate the MMU execution counter further | Rejected. Linked register carry regressed; separating the MMU sequence fields gave mixed results and the original layout stays. |
## x86-64 gadget engine campaign

Measured on macOS with matched ABBA runs: focused guest probes for the
mechanism, then single-core Geekbench 6 for the workload. X1 through X13 were
settled together over the full suite; later entries were each measured
against their parent on the workloads their mechanism reaches.

| ID | Attempt | Disposition and lesson |
| --- | --- | --- |
| X1 | Declare the ARMv8.5-A host baseline | Accepted as an enabler with no performance claim. The engine is macOS-only, so every host implements LSE, FlagM, FlagM2 and FRINTTS. |
| X2 | Drop result normalization the host already performed | Accepted. A W-form host instruction writes the whole 64-bit register, so width normalization was a no-op at 32 and 64 bits. |
| X3 | Fold away the page-crossing test for single-byte accesses | Accepted. A one-byte access cannot cross a page. |
| X4 | Write the deferred-flag tail as one constant | Accepted. Operation and width are macro parameters and the rest of the tail is zero, so eight bytes are one link-time constant. |
| X5 | Interprocedural optimization for the translation path | Accepted on workload evidence against probe evidence: probes cannot see a change that only affects translation. |
| X6 | Resolve effective addresses without branching on the descriptor | Accepted. Every address field is a translation-time constant; an always-zero state slot lets an absent base or index be added unconditionally, so eight branches become none. |
| X7 | Extract byte sign masks with NEON | Accepted. Four shift-and-accumulate steps replace a per-lane loop in glibc's SSE2 string routines, which a static histogram of a dynamically linked benchmark cannot see. |
| X8 | Skip the store-side publication and overlap check | First attempts rejected: Linux grants READ_IMPLIES_EXEC to binaries without a non-executable stack marking, so every data page qualifies as executable and gating on the write-code or executable-page bit skipped nothing. |
| X8b | Call code publication only when a store can publish something | Accepted. Publication was over half a guest store's cost and its call misses whenever the page holds no cached translation; recording that in the write translation made stores about twice as fast. The expensive part can be the call a sequence guards. |
| X9 | Evaluate branch conditions from the deferred record | Rejected. A conditional branch's flag materialization already exits early; a pre-check ahead of it made branches much slower in two implementations. |
| X10 | Fold the recorded operation and width into flag materialization | Accepted. The materializer rediscovered operation and width, constants at the recording site, through run-time chains; one bounded table branch now selects a body with both folded in. |
| X11 | Discard the deferred record in the bit-count family | Accepted. TZCNT, LZCNT and POPCNT define every arithmetic flag and cleared all six right after materializing them; two stores replace the call. |
| X12 | Evaluate carry conditions from the deferred record | Rejected, X9 from the other direction. The zero shortcut works because a recorded result is already width-masked; carry needs the recorded operation, so the shortcut reintroduces the dispatch it avoids. |
| X13 | Stop advertising ERMS | Accepted at the time, superseded by X15. While REP ran per element, withholding ERMS moved string work onto vector gadgets: a capability claim is a scheduling hint, so it is a performance decision too. |
| X14 | Move a repeated string operation in page-bounded runs | Accepted. A REP MOVS/STOS run within one source and one destination page pays address resolution, TLB probe and publication once and moves 64-byte blocks. The first version fell back whenever the destination page could hold code, and gained nothing. |
| X15 | Advertise ERMS again | Accepted, superseding X13. Once X14 made REP a block move, REP MOVSB/STOSB outran glibc's vector routines, so the bit went back for guests that assume it. |
| X16 | Keep the native chain alive across a code publication | Accepted, the largest result of the campaign. A code publication reset every link in the address space, so the engine left an already resident stream every few instructions; re-arming a record whenever its slot is returned raised instructions per stream entry about sixfold. Counting stream entries exposed it. |
| X17 | Materialize only the carry for the operations that need it | Accepted. INC, DEC, ADC and SBB called the full flag materializer though only CF is live; a carry-only entry serves register forms, since a memory form can fault while stale bits are visible. |
| X18 | Read a condition from the flags the compare already set | Accepted. After a flag-setting 32/64-bit CMP, SUB, TEST or AND, NZCV already holds the x86 flags, so the builder pairs producer and condition consumer through one variant per condition. A probe of pure block transitions regressed: it measured what the change does not address. |
| X19 | Advertise fast short REP MOVSB | Accepted. glibc reads FSRM separately from ERMS to send short copies to REP MOVSB, and after X14 that claim holds: a neutral measurement is no reason to route the guest around a path that is not slow. |
| X20 | Widen the indirect-branch site cache from two targets to four | Rejected. Four ways per indirect site sped a four-caller probe but not call-heavy workloads: the mechanism working is not the same as the change paying. |
| X21 | Keep a call in the stream when it publishes a code write | Accepted, the largest result since X16. A per-site exit census found a linked CALL leaving because its own return-address push published a code write; a call now continues like any store unless the write lands in the running block. |
| X22 | Link the block end the fetch window created | Accepted. A block cut short by the fetch window ended in an unlinked fall-through, the largest remaining exit cause; the next address is as static as a jump target, so it asks for a link. |
| X23 | Keep indirect-site training across a link cooling | Rejected. Retained indirect-site training then follows a dead link, so the exit moves one category over: moving an exit between causes is not removing it, and a census of causes settles that on a loaded machine. |
| X24 | Key the indirect-branch tier on the target rather than on the branch | Accepted. A return site belongs to the call that reached it, so N callers mean N return targets competing at one site (why X20 failed); one direct-mapped tier keyed on the target and shared across the address space holds them all. |
| X25 | Retire only the translations a code change covers | Accepted. Replaying unseen code-change ranges through selective retirement stopped a code change anywhere from cooling every link. A `std::shared_mutex` on the code-write path cost more than it saved (libc++ notifies on every unlock); a sequence-stamped lock-free ring replaced it. |
| X26 | Reopen the instruction window instead of leaving at it | Rejected. In-stream interrupt and code-change checks at window checkpoints removed half the remaining exits but were flat on Geekbench and slower on Chromium: exits are not fungible. |
| X27 | Publish a code write only where some executor caches code | Accepted. Under READ_IMPLIES_EXEC every readable page may hold code, so data stores forced every executor to revalidate; a per-generation registry tags a page as code exactly when a translation of it exists. An imprecise publication costs every executor's reaction. |
| X28 | Bake registers into a generated scalar bank and read host flags at every width | Accepted. A generated register-baked scalar bank has immediate offsets and bases, which the host forwards through (G196), and host NZCV at every width widens fusion. Gains fell well short of the ceiling bench: scale a probe gain by the share of run time it touches. |
| X29 | Record shifts and overlay multiplies instead of materializing | Accepted. Shifts record their own deferred operation and multiplies overlay carry and overflow. Summed projections overshot because each assumed halving work that was a fraction of the total. |
| X30 | Carry host flags across a flag-neutral transfer | Accepted. A flag-neutral MOV, MOVZX, MOVSX or LEA between a compare and its consumer passes the host flags through; a memory destination still ends the fusion, since a store's epilogue compares addresses. |
| X31 | Retire only the TLB entries a mapping change covers | Accepted. Every engine entry cleared the whole software TLB; replaying the unseen mapping ranges (X25's service) retires only covered entries. |
| X32 | Take the preserved carry from host flags in INC and DEC | Accepted. A variant per carry source lets INC and DEC read the carry in one condition when the builder knows the producer. The suite barely moved: a large share of very little. |
| X33 | Translate a conditional branch's fall-through inline | Accepted, the largest single change. Continuing the decode past a conditional branch puts its fall-through in the same stream (arm64's G162) and speeds the taken shape because blocks grow. |
| X34 | Bias the retirement counter by the dispatch budget | Accepted. Counting retirement from minus the dispatch budget makes each linked edge's budget test a sign test (arm64's G180 inverted). |
| X35 | Run the SHA-256 message schedules on the host SHA unit | Accepted. SHA256MSG1/MSG2 map onto the host's SHA256SU0/SU1, several times faster and bit-identical. The rounds stay scalar (x86 does two where the host pair does four) and SHA-1 stays scalar (x86 numbers words from the high lane). |
| X36 | Publish the guest rounding mode for the whole dispatch | Accepted. The dispatch publishes the guest's MXCSR rounding mode into FPCR at entry and on every MXCSR write, so conversions use `frinti` (arm64's G30). Restoring a saved FPCR at leave restored garbage after a faulting gadget changed the stack pointer; deriving it from FPCR needs no saved copy. |
| X37 | Bake the vector register and base into 128-bit transfers | Rejected. Baking vector register and base into 128-bit transfers sped the [base + displacement] shape but not the indexed shape SIMD loops emit, and its text is paid by every workload: bake the shape the workload uses. |
| X38 | Fold the page-crossing test into the TLB tag compare | Rejected on correctness before measuring. A range check cannot replace the tag compare and crossing test: a reset entry falsely hits a window above its tag. A tag compare carries the invariant that this entry describes this page. |
| X39 | Leave a NOP untranslated | Rejected. Eliding NOPs broke the one-retirement-per-instruction contract that budgeted replay and the K-trace corpus pin; the retired count is the unit of guest progress. |
| X40 | Discard the pending record in operations that define every flag | Accepted. Instructions that define every flag they do not leave undefined (COMISS, PTEST, packed string compares, bit scans, BMI writers) clear the pending record with two stores instead of materializing it. |
| X41 | Write scalar vector moves with one full-width store | Rejected. One full-width store in place of a zeroing wide plus narrow store was inside the noise; this core does not stall on a wide load after a narrow store at this distance. |
| X42 | Keep AESKEYGENASSIST in the vector unit | Accepted. AESKEYGENASSIST stays in the vector unit; kept for the probe gain and shorter body though its profiled share did not survive the suite. |
| X43 | Round PMULHRSW with SQRDMULH | Accepted. PMULHRSW uses SQRDMULH with the one saturating lane (both inputs 0x8000) flipped back by a mask. With X42, a profiled share that did not survive the suite: per-gadget attribution is suspect for memory forms and shared tails. |
| X44 | Give the software TLB four times the reach | Accepted. The direct-mapped software TLB grew fourfold (32768 entries, 1 MiB); probes understated the suite because a benchmark loop touches pages in order and real workloads interleave them. |
| X45 | Park the carry INC and DEC preserve instead of settling it | Accepted. When the carry's producer is unseen, INC and DEC park the pending record in a carry slot and only a reader of the carry computes it: a flag no one reads costs nothing. |
| X46 | Carry host flags across a baked memory transfer | Accepted. The baked bank's loads, stores, PUSH and POP leave NZCV intact so a fused condition reaches its consumer across them; the ordinary bank still ends a fusion, as only baked entries were audited. |
| X47 | Fix the generated NaN without a branch | Rejected. A branch-free mask for the indefinite-NaN fix made every FP result slower: a branch whose body is skipped is what makes the body free. |
| X48 | Re-check the stack's own TLB entry | Accepted. The execution context keeps the TLB entry the last stack access used and returns the host address on a matching tag, sparing PUSH, POP, CALL and RET the hash and table read. |
| X49 | Take a vector operand from the register the gadget before it left it in | Accepted. A vector integer register form's result stays in host `v4` (`v5` at 256 bits), and a builder-set descriptor bit says the gadget before wrote exactly this register and width, so dependent chains take the operand from the register. An unsound always-forward build measured the ceiling first. |
| X50 | Forward vector results in the map2 and floating-point families too | Rejected. The same forwarding in the map2 and FP families doubled their probe but lowered the suite: the test sits in front of every gadget and those families chain too rarely. Only the suite separates one family from the next. |
| X51 | Read the x86-64 program cache counters on Clang | No change. Counters on Clang read a quarter of lookups ending in a fresh translation as cold compiler code; X58 corrected that, because the census never counted evictions. |
| X52 | Feed a vector memory operand from the baked address entry | Rejected. A second entry joining past the address decode changed nothing: the extra dispatch costs what the decode did. |
| X53 | Release unused x86 translation capacity | Accepted. Short blocks retained the decoder's 64-instruction allocation in every vCPU cache; compacting streams before publication makes retained storage follow block length. A storage reduction, not a throughput gain. |
| X54 | Order guest memory accesses across vCPUs | Accepted as a correctness requirement. Without total store order and LOCK's full fence, multi-threaded guests crashed or deadlocked because the host really reorders; a `dmb ish` on the way out of every gadget touching guest memory costs under two percent and nothing cheaper exists. |
| X55 | Make the code-page registry's membership exact | Accepted, correctness-of-performance. The code-page registry counted per hashed bit with a neighbour-adjacent hash, so a run of code pages could mark a thread's stack as code and every vCPU retranslated every block; an exact count per page fixed it. A conservative filter needs cheap false positives. |
| X56 | Keep a cached translation across another vCPU's code change | Accepted, the other half of X55. Validation discarded a cached program whenever the code-change sequence moved; taking each page stamp once, before the bytes it certifies, makes it a lower bound. Take a freshness token before the data it certifies. |
| X57 | Publish a code change for every executable page a program stores into | Accepted as a correctness fix. Publication was keyed on the program, so a store into a second executable page published nothing and other vCPUs kept a stale translation: a suppression keyed on the actor is sound only when what it protects is also the actor's. |
| X58 | Size the private program cache from the working set rather than from one number for every product | Accepted, with arm64's G208. Eviction-attributed counters showed multi-core Clang's executors at full occupancy against a working set about three times their slots (correcting X51): a cache at full occupancy against a much larger working set is one to enlarge, not tune. |
| X59 | Choose the victim way by second chance instead of round robin | Rejected, after measuring the ceiling first. Shadow caches on the real reference stream showed a policy that learns only from lookups (resolved links never look up) could save little. Simulate the achievable ceiling before building a replacement policy. |
| X60 | Build a translation in buffers the thread keeps, and allocate the finished block | Accepted, in a shape the suite corrected. Per-thread build buffers and an exact-size copy-out cut translation cost, but one merged allocation moved hot streams among cold data and cost the browser workload; merge allocations only when touched together. |
| X61 | Fetch an instruction's bytes in one page-bounded window | Accepted. The decoder fetches up to fifteen bytes in one page-bounded window instead of a call per byte; a test pins both halves of the page bound. The cost of a byte-granular reader is the call. |
| X62 | Put a translation's saved source bytes in the translation | Accepted. A translation's saved source bytes live at the end of the allocation holding its decoded instructions: allocate what is produced with an object and meaningful only beside it with the object. |
| X63 | Answer a repeated code-page handle without a lock | Accepted. The code-page record lookup sat behind libc++'s serializing `std::shared_mutex`; records never move once created, so a lazily filled two-level index of atomic pointers answers repeats. A reader-writer lock is the wrong tool for a table that only grows. |
| X64 | Fetch a whole block through one window | Accepted, smaller than it looks. One 128-byte window shared across a block's decodes cut fetches to about one per block but translation cost only slightly: a sample's attribution to a symbol does not say which of its calls are expensive. |
| X65 | Take a packed-integer operand from either side of the gadget before it | Rejected. Forwarding the right operand too, where X49 forwards the left, left the suite unmoved though the profile put Object Detection's time on that load. An engine test pins which bit forwards which operand. |
| X66 | Forward from every producer, and into the floating-point family | Rejected, and X50 with it. Forwarding from every producer and into the FP family grew text and moved no workload: a sampled operand load is where the machine waits, not what it waits for. |
| X67 | Write a 256-bit vector result as one store pair | Rejected. One `stp` for a 256-bit result was slower across the suite: the next gadget's 16-byte operand load cannot forward from a 32-byte store. |
| X68 | Load a 256-bit vector operand as one pair | Rejected. With X65 to X67 this settles that these gadgets are not short of operand bandwidth or latency; the vector gap lies in how many gadgets a 256-bit instruction costs. |
| X69 | Keep a translation's bytes, and its decoded instructions only where a replay asked | Accepted. A translation carried about 1.8 KiB of decoded instructions whose only reader is the budget-limited replay; it now keeps stream and source bytes, a replay decodes back, and only a block replayed before keeps its decoded form. Dropping the copy outright lost 2 percent: "a rare path" is a measurement, not a reading of the code. |
| X70 | Stop a plain long-mode address once its components are summed | Accepted. In long mode with no segment override and no narrow address size, masking and adding a segment base are identities; the builder proves that and sets one descriptor bit, and the general tail moved to `__gadget_cold`. A provable bit beats a variant per shape only when the unserved shape also leaves the hot stream. |
| X71 | Leave a read-only memory gadget through an acquire barrier | Rejected. Under total store order a read-only gadget could leave through `dmb ishld`, but it bought nothing: measure an ordering relaxation against a barrier that actually costs something. |
| X72 | Settle a store's publication in five instructions | Accepted. A store's publication decision took eleven instructions; the low bit of `write_code` now joins the cached-code bit in one test and the rare arm moved to `__gadget_cold`. The gain is in what the bank no longer fetches. |
| X73 | Let the block that crosses a dispatch's instruction budget run to its end | Accepted. A block now runs to its end past a dispatch budget, and only a single step and the instruction after a TF-setting POPF ask for an exact budget; exactness a caller does not need is paid for twice. |
| X74 | Keep the x86 cache's link records and indirect sites in G210's chunked table | Accepted. The x86 cache moved its link records and indirect sites into G210's shared chunked table, saving a little memory; kept because both engines now share one record store. |
| X75 | Carry a memory access's RIP in the stream word that carries its length | Accepted. A baked memory access carries its RIP (48 bits) and length in one stream word only the fault and overlap exits take apart; an access whose RIP does not fit takes the unbaked form. |
| X76 | Name a stream's gadgets by 32-bit offset instead of by pointer | Measured, with arm64's G212; not built. The one `and` a 32-bit offset adds to every dispatch cost a few percent. The lesson is G212's. |
| X77 | Name a way's successors and code-page neighbours by 32-bit slot index | Accepted. Naming successors and code-page links by 32-bit slot index cut a way by a third; what every way carries is priced by capacity, not occupancy, and at the macOS slot count this cache runs half empty. |
| X78 | Let link_exit read the RIP out of the BlockLink behind its stream word | Accepted. `link_exit` reads the target RIP from the BlockLink, so a link record is 64 bytes and no longer straddles a 128-byte line: size a record the linked path reads to divide the line. |
| X79 | Order the x86 Program's members widest first | Accepted. Ordering the Program's members widest first removed padding, and a 256-way slab then fit a smaller allocator granule, so peak RSS fell far more than the byte census predicted; `malloc_size` says what a block costs. |
| X80 | Keep what only an occupied x86 way needs out of its slot | Accepted. What only an occupied way needs moved into a record allocated on its first translation, leaving a 64-byte slot: what a way needs only while occupied should cost only while occupied. |
| X81 | Name the x86 record tables' entries by record number | Accepted, with arm64's G214, which changed the table both engines share. The lesson is G214's. |
| X82 | Leave the terminator out of a block that ends at a branch | Accepted. A block that ends at a branch no longer carries a `block_exit` terminator; only a block the fetch window cut short needs one. |
| X83 | Gather the hottest x86 gadget entries into one page-aligned section | Rejected, with arm64's G217. Gathering the hottest entries onto a few pages cut L1I misses but left IPC and Geekbench unmoved. The lesson is G217's. |
| X84 | Replace SSE arithmetic fix-ups with FEAT_AFP host modes | Measured, parked; it could serve only the macOS x86 slice on M4 and later hosts. FPCR.AH, NEP, FIZ and FZ could replace lane merges and NaN repair, but gains were small and limited to dependent scalar chains and NEP gives one-input scalar instructions a false dependency. A sample share is not removable work. |
| X85 | Carry a baked access's displacement in its access word | Accepted. One access word holds a baked access's displacement, block position, length and a 14-bit immediate, and cold exits rebuild the RIP from the block's start. Recompute a word only a cold exit reads. |
| X86 | Give an x86 way a lookup found a second chance before its set replaces it | Rejected, with arm64's G230: the hit rate gain was a tenth of a point for a larger cache. The lesson is G230's. |
| X87 | Give each x86 gadget cache set eight ways instead of four | Kept, with arm64's G231, whose geometry it shares; census builds hit more often at 262,144 and 131,072 slots and timing held level. The lesson is G231's. |
| X88 | Pack an x86 gadget cache slot and its successor set into 40 bytes | Accepted, with arm64's G233. An x86 way's slot is four words (address-and-mode tag under a 16-bit address-space id, translation, link memo with a 16-bit epoch, index and flags) and a successor set 8 bytes. |
| X89 | Shrink an x86 translation record from 112 to 96 bytes | Accepted. A program held its block's byte count beside a saved source of exactly that length; reading one from the other, with stamp bookkeeping in the slot's spare byte, makes a record a whole number of allocator sixteens. |
| X90 | Keep what an x86 gadget cache's ways evict in a packed victim tier | Rejected, with arm64's G235; timing was not run once arm64 showed restores do not buy back what a smaller cache loses. The implementation is not retained. |
| X91 | Carry an ordinary x86 instruction's position in a word its gadget already reads | Accepted. An ordinary-bank gadget that can fault took its 64-bit address from a stream word only cold exits read; it carries a 10-bit block position in spare bits of a word it already reads. One gadget's alignment phase alone can move a probe by 9 percent. |
| X92 | Fold small x86 operands into the ordinary descriptor | Measured; not built. The ordinary bank's small operands are under 1 percent of stream words, and extracting an immediate from an offset word costs an instruction in every execution (G212, X76): a fold must pay in executed instructions as well as words. |
| X93 | Leave a baked x86 PUSH and POP no stream word | Accepted. A baked PUSH or POP no longer reads a stream word only its exits used; they store the stream position and the runtime decodes the block's kept bytes. Baked stack faults are rare. |
| X94 | Release the x86 link records and sites nothing names any more | Accepted, with arm64's G256, under the same rule. A released record is unlinked from the live list first, and a target-table entry naming it reverts to vacant. |
| X95 | Let go of the x86 gadget spaces of address spaces the kernel destroyed | Accepted, with arm64's G272. An x86 executor's cache is private, so the executor that ran a destroyed generation holds all of its state; `run_x86_gadgets` gets the MMU's destroyed-generation count and an injected answer function (no new edge to the MMU) and drops destroyed spaces. |
| X96 | Index the x86 gadget cache sets by one multiply of the key | Accepted. Guest code clusters at alignments and strides and the two-xor-fold set index is linear, so a parallel compile left over a thousand resident sets full where a uniform spread has 128; the set is now the top bits of the key times a 64-bit constant. Judge an index by recomputing both from a census, not hit rate alone. |
| X97 | Keep an x86 translation in an arena body that its address space's Space owns | Accepted, groundwork for sharing (X98) after the arm64 sharing machinery moved to `darwin/cpu/`. A translation is a 128-byte body in a `BodyArena` with its stream behind it, and an address space's arenas, links, sites and target tables are its `Space`; judge a restructuring on bytes, hit rate and score. |
| X98 | Share an x86 address space's translations among the executors that run it | Accepted, behind `DARWIN_UML_GADGET_SHARED_BODIES=1` until X99. Executors of one address space share a `Space`, a link is one aligned pair retargeted by a single paired store (FEAT_LSE2), and a released body waits in limbo until every executor seen in translated code has left. The census peak fell 60 percent. |
| X99 | Renew a shared x86 address space once its arena is mostly freed, and share by default | Accepted. A shared space is never swept, so under churn its arenas kept holes; once they hold at least as much freed space as bodies and at least 2 MiB it is closed and the next executor gets a new one. Sharing is then the default (`DARWIN_UML_GADGET_SHARED_BODIES=0` keeps translations per executor). A bound a cache cannot reach is no bound. |
| X100 | Replace an occupied entry of a shared x86 target table one time in eight | Measured; not built. A shared target table is written more often, but a same-binary throttle moved no score: a write count is not a cost until a same-binary switch moves the score. |
| X101 | Advertise AVX-VNNI to x86-64 guests | Accepted, on both engines. CPUID leaf 7 subleaf 1 EAX bit 4 is set; ggml's AVX-VNNI variant took 7 to 10 percent less time. The JIT runs a 256-bit loop about twice as slow when operand arrays are misaligned relative to each other: judge a CPUID bit on its consumer's kernels, with a placement control. |

The opening profile ranked candidates by what executes rather than by a static
instruction histogram, and so retired several planned items that barely ran.
Its largest item, leaving and re-entering the stream, was answered by none of
the planned structures: counting exits by cause found the existing chain
machinery switched off wholesale (X16), and later exit censuses drove X21 to
X26. What remains of stream re-entry is exits that miss the program cache or
re-arm a cooled link, so further work belongs in translation and validation.

Five lessons generalize. Whether a page might hold code is not a condition this
engine can branch on to save work: the predicate is true far more often than
publication is needed and cannot be predicted from the guest's own markings, so
the work has to become cheaper rather than conditional (X8, X14). Instruction
count inside a gadget is a weak predictor: the changes that paid removed a
loop, a call or a branch. A probe measures only the mechanism it names, so a
candidate that affects translation, or trades throughput for latency, needs a
workload to judge it. Laziness is worth paying for only where retrieving the
deferred value is cheaper than computing it (X9 and X12 against X10). And a
profile ranks what runs but not how often a structure is bypassed: count how
often a fast path is taken before assuming its cost is the thing to reduce.


## x86 equality conditions in Wine

| Attempt | Disposition and lesson |
| --- | --- |
| Avoid full flag evaluation for equality-only consumers | Accepted. CMOVE/CMOVNE read the deferred zero result and adjacent 8- and 16-bit CMP, SUB, TEST and AND consumers reuse the host zero flag. It removed startup work from a timed game launch near its deadline; not a reliability claim. |
| Reuse narrow subtraction carry conditions | Accepted. Zero-extended byte and word CMP/SUB operands preserve host unsigned ordering, so adjacent carry-dependent branches and SETcc can use NZCV; signed and parity conditions keep the deferred path. |

## Graphics backend investigations

| Attempt | Disposition and lesson |
| --- | --- |
| Separate CGL/OpenGL and mobile renderer paths | Replaced by one authenticated host GL provider across Apple products, now Mesa Zink over KosmicKrisp. |
| Complete GLES 3.2 through existing APIs | Accepted once the host provider became Zink over KosmicKrisp: Virgl on its desktop GL 4.6 context gives the guest OpenGL ES 3.2 and the Android extension pack through Metal ([`../android/lineage-24/README.md`](../android/lineage-24/README.md#graphics)). |
| Add Vulkan by pairing Venus with MoltenVK | Not implemented. MoltenVK does not supply the missing Virtio-GPU blob/shared-memory protocol, synchronization, or lifetime model. |
| CPU readback as native presentation | Retained only as a diagnostic fallback; sustained native qualification requires the Metal texture path. |
| Profile Chromium H.264 playback | Accepted. Fence polling and VNC scanout readback dominated the GPU thread while decode was small; packed damage readback, persistent staging, predicate-aware fence polling, reusable codec buffers and NV12 IOSurface/Metal copies raised delivered frames by about half. |
| Defer Virgl fence completion | Accepted. Host-owned per-context timeline cookies keep multiple fenced submissions and transfers in flight without blocking the GPU transport thread; renderer-state mutations remain barriers. |
| Post-deferred-fence Chromium H.264 candidates | Rejected under a 1080p60 A-B-B-A protocol: none cleared the screen. VideoToolbox and fence waits are mostly blocked wall time while vCPU execution dominates actual CPU. |
| VideoToolbox H.264 candidate sweep | Rejected under the same protocol; direct decoder-output aliasing is invalid at the resource-semantics gate, so the synchronous decode and Metal-copy path stays. |
| Use the native JIT for Chromium H.264 playback | Rejected. Translation-cache churn stalled this browser workload before playback, so the qualified gadget engine remains the performance baseline. |
| Gadget-cache and TLB follow-up for Chromium H.264 | Rejected. Program caches were nearly full with heavy translation churn, but every link, hash, capacity, associativity, replacement and TLB variant regressed or broke boot; retaining address-space-local state needs a correct generation and lifetime design. |
| Extend VideoToolbox decode to HEVC and P010 | Accepted. Hardware HEVC Main/Main10 decode was an order of magnitude faster than software with almost no guest decode CPU; presentation and guest GPU work remained the 60 fps bottleneck. |
| Extend stock-guest VideoToolbox formats | Accepted HEVC Main/Main10 up to 8K and JPEG Baseline/MJPEG. H.264 High10 was rejected after a false-positive session probe, and 4:2:2/4:4:4 because stock Debian libva and Mesa expose no such profiles; no guest-library fork is supported. |
| Decode straight to packed RGB in the VA-API backend | Accepted. Blitting a guest-converted NV12 frame cost about 150 ms a 1920x1200 frame against about 1 ms to copy a device-produced packed `AR24` frame: the guest conversion's cost is scalar arithmetic, not memory bandwidth. |
| Lend the capture queue the VA-API driver's surfaces | Accepted for the avoidable copy, not for speed: copying a frame out of a capture buffer is a fraction of a millisecond beside decoding it. |
| Bound KosmicKrisp Metal retention by Vulkan lifetime | Accepted. Finite raster variants, owner-based pipeline retirement, command-allocator reset and completion-based release stop storage growing with submission count; guest RAM limits do not bound native graphics allocations. |
| Avoid raster expansion when rasterizer discard is enabled | Accepted. A dedicated discard class keeps transform feedback and shader side effects and drops unused raster output; discard does not permit dropping earlier programmable stages. |
| Recycle KosmicKrisp command storage between submissions | Accepted. One-time-submit recordings released their Metal command allocators on completion, so each submission bought storage from the kernel; a refcounted device-level allocator cache cut submission latency about threefold. Command storage belongs to the device. |
| Hand a driver only the sample mask bits that select samples | Accepted. KosmicKrisp folds the sample mask into its fragment function, so Zink paid a Metal compile per mask value, including bits past the framebuffer's samples; a dEQP test randomizing them ran past the CTS watchdog. Reduce state a driver compiles into code to the part that changes the result. |
| Alias an imported shared-memory blob instead of copying it | Accepted. A mixed-mode guest re-uploaded every imported surface before each read; `EGL_MESA_image_host_pointer` lets the importer alias the exporter's pages (falling back loudly to a copy), cutting host CPU per frame by about a tenth. |
| Make the A15's faulting fragment loops cheaper | Two accepted, six rejected, premise withdrawn. `constant`-space descriptor loads and unpacked aligned vector accesses cut per-iteration work; other shader levers changed nothing. A native Metal probe showed the event is a GPU fault tracking fragment-stage store work, not elapsed time (see [`graphics.md`](graphics.md)). |
| Allocate the CPU presentation copies only when the CPU path uses them | Accepted. Every applied guest mode allocated the CPU framebuffer, its Metal texture and Virgl readback staging at full scanout size, which a Metal-textured scanout never uses; allocating on first CPU use cut the iPad's footprint by about 100 MiB at 4096x2160. |
| Retire cleared fallback textures and convert pixels directly into the framebuffer | Accepted. Publishing a clear releases the obsolete CPU fallback texture, and nonoverlapping conversion uses one SIMD pass without full-image staging. CPU-path savings only. |
| Retire the fallback texture once a native frame replaces the CPU path | Accepted. Only a clear released it, so the texture the boot console's frames made stayed for the guest's lifetime after its GPU scanout took over: a display-sized shared allocation, about 14 MiB of the GPU pool at 1320x2580. The framebuffer keeps the pixels and every CPU draw uploads all of them, so going back to the CPU path costs one allocation. |
| Reserve a tessellated draw's domain points once per draw | Accepted. With compare-exchange heap reservations every patch contended on one fill counter; a draw's points are now reserved at once. A per-invocation atomic on one shared counter serializes a large dispatch. |
| Free Zink's cached buffers when they expire | Accepted. `pb_cache` frees an expired buffer only on the next add or reclaim, so an idle screen keeps its whole cache; one reaper thread per screen frees expired buffers on a timed wait. |
| Cap Zink's unused buffer cache at 32 MiB | Accepted, cost unmeasured here. The cache was bounded only by an eighth of the heap; the bound is now the smaller of that and 32 MiB (`ZINK_BO_CACHE_MAX_MIB`). Frame times belong to the performance queue. |
| Report a smaller KosmicKrisp memory heap on request | Accepted as a measurement switch. The heap is all of system memory, so Zink's cache follows the machine; `MESA_KK_SYSMEM_HEAP_MIB` reports a chosen size, never above the machine's. |
| Give a macOS measurement run the container-file segments of iOS | Accepted as a switch, not a default. `shm_open` is denied on iOS, so texture exports and Venus blobs are unlinked files outside the footprint, but on macOS they read high; `VIRGL_ANON_FILE_UNLINKED=1` takes the file. Dirty file pages can be written back to the volume. |
| Measure the presentation memory without a window | Accepted. The iPad allocates drawables and a frame-mirror pool a VNC run never does, and `--gpu-display=native` opens a window on a shared Mac; a windowless layer with three drawables and a three-buffer mirror pool matches it. |
| Trim a guest GL context that has gone idle | Accepted, no cost to a submitting context. A Zink context keeps completed batch states and their resources until its next batch, and glthread keeps a deleted texture's last reference until the next fence, so an idle guest held up to five full-screen heaps. One second after the last command the renderer flushes and `eglTrimContextMESA` resets completed states: release idle state by explicit trim. |
| Offer ETC and ASTC to the guest on a desktop GL host | Accepted. virglrenderer listed them only for a GLES host, so guest Mesa decoded every ETC or ASTC texture to RGBA8 at four or more times its size. Its ASTC sRGB entries also carried the linear formats' numbers. |

## Guest timer policy

| Attempt | Disposition and lesson |
| --- | --- |
| Enable high-resolution timers on the mainline kernels | Accepted; Android already had them. One-millisecond sleeps became twice as accurate with host CPU neutral; the 100 Hz tick policy, not timer resolution, bounds host wakeups. |

## Kernel frame pointers

| Attempt | Disposition and lesson |
| --- | --- |
| Build every Darwin kernel with frame pointers for the stack walker | Accepted as a correctness requirement: the walker follows frame records, so traces list only reliable entries. It costs about 1 percent on the iPad and about 4 percent kernel text. |

## 4 KiB native MMU page publication

| Attempt | Disposition and lesson |
| --- | --- |
| Read the MMU trace switches once per process | Accepted. The per-page trace checks called `getenv` three times, which slowed exec-heavy work; a sampled 2 percent share understated the effect. |
| Reach guest RAM through the mapping's recorded backing identity | Accepted. Each shadow-page read or write called `fstat` three times to recognize and size guest RAM, although a mapping already records the backing's device and inode. |

## Kernel uaccess bridge

| Attempt | Disposition and lesson |
| --- | --- |
| Repair a stale uaccess mapping with one bridge call | Accepted with bridge ABI 14. A uaccess page asked whether the host mapping matched, then made a second call that also published 4 KiB shadows; `darwin_uml_vm_discard_if_stale()` replaces both and publishes nothing. |

## Kernarium startup display

| Attempt | Disposition and lesson |
| --- | --- |
| Replace direct Python/Cairo window drawing with C++ GPU composition | Accepted. Cache glyphs and scene geometry, animate through shader parameters and publish complete EGL/GBM frames; unchanged progress needs no redraw, and status polling stays separate from animation cadence. |

## Layout-sensitive cleanups

Cleanups that move data layout on the dispatch path were held back until
measured against a same-day control, gadget changes on the iPad and JIT changes
on the Mac, and landed only when every probe and workload was neutral or faster.

| Attempt | Disposition and lesson |
| --- | --- |
| Remove the gadget body arena's counting resource | Rejected. The wrapper is `ProgramCache`'s first member, so removing it shifted every member's cache-line phase and slowed the bimodal indirect-link probe: a dead member at the head of a page-aligned hot structure is layout. |
| Drop `ProgramWords`' zero-length inline buffer | Rejected. Probes were level but Navigation lost in the Geekbench subset and was level alone; the loss follows the translation cache's state after the preceding workload, which steady-state probes do not exercise. |
| Pin the gadget region at the head of each image's text and pack the bank | Accepted, bank phase 48. On the iPad a probe's fast or slow mode is chosen per launch partly by the address dyld loads the runtime at, and the region's place moved with unrelated size changes; with the region first in `__text`, every product carries the same region bytes at the same offset. Probes still swing with layout elsewhere. |
| Pin the x86-64 gadget region and give each engine group a fixed slot | Accepted. One x86 gadget change moved nearly every gadget by 2,480 bytes and cost File Compression and Clang 2 to 4 percent with no code change, until a pad restored each offset modulo 128. The region starts on a 16 KiB page after the arm64 region and each group has a slot of whole 128-byte lines (`fit_slots.py` lists slots that no longer fit); pin each group's offset, not only the region's start. |
| Read host capability flags from the feature set | Rejected. Replacing twelve duplicated booleans with accessors sped the JIT probes but cost Navigation in every gadget launch: a byte-identical bank object does not pin the bank's address. |
| Remove the JIT's unused GPR residency planner | Accepted. Production never planned, but the planner's members made `JitCompiler` large enough that `run_block` called the stack probe on every entry: a large object on the dispatcher's stack costs every block entry. |
| Count JIT block links once | Accepted as level. Dropping a counter that always equalled the size left every probe within about 2 percent. |
| Recycle the JIT cache when a link table fills | Accepted. Link records keyed by address space were reclaimed only with a full arena, so short-lived processes filled the tables first and later blocks published unlinked exits, a tenfold slowdown. A bounded table keyed by a transient identity needs its own reclamation path. |
| Fold the arm64 gadget cache's constant epoch into slot state | Accepted as level. The epoch was 1 and nothing wrote it, so the slot fields only repeated whether other pointers were set; a slot shrank from 56 to 40 bytes. A tag nothing writes is state held for nothing. |

## Guest pointer authentication

| Attempt | Disposition and lesson |
| --- | --- |
| Implement and advertise FEAT_PAuth with a cheap FEAT_PACIMP algorithm | Rejected; PAuth was removed. Even with PACIxSP+STP and AUTIxSP+RET fused in the gadget engine, a four-round AES-based PACIMP cost 1.39 percent on llc on the iPad, since frame signing runs on every non-leaf call and return. Hint-space forms stay NOPs. |

## Floating-point control features

| Attempt | Disposition and lesson |
| --- | --- |
| Select native FHM, BF16, FP8, and half-precision dot and matrix gadgets on hosts without FEAT_AFP | Accepted. The gates ran these families through software helpers so a guest's FPCR.AH, FIZ or NEP would behave as with FEAT_AFP; the guest can no longer set those bits, so native instructions are exact and far faster. A fallback that only models a RES0 control goes with it. |
| Select native BFDOT and BFMMLA gadgets on hosts without FEAT_EBF16 | Accepted. With FPCR.EBF RES0 these ignore FPCR (round to odd, flush subnormals, default NaN, as the A15 does) and matched the software model bit for bit; BF16 GEMV and GEMM ran over twenty times faster. |
| Skip the JIT's FPCR.NEP destination merge on FEAT_AFP hosts | Measured; the merge stays. A probe ran 31 percent faster without the old-destination load, but Geekbench FP workloads ran slightly slower: the merging forms are under 5 percent of scalar destinations. A probe's gain predicts a workload only where the probed form is dense. |
| Give the x86-64 JIT x86's SIMD NaN rules and DAZ through FEXCore's FEAT_AFP path on M4-class hosts | Measured; not adopted. NEP made a one-input scalar conversion into a reused register about ten times slower on the M4, and AH/FIZ replace only plain arithmetic's tests while the M1 needs all of them. A host mode covering some cases leaves the check in place and adds a configuration to validate. |
| Trim the x86-64 gadgets' masked-flag work under MXCSR.DAZ and FTZ | Rejected. Dropping a branch per sum, difference, product and quotient left kernels 1 to 2.5 percent slower, inside the unchanged multiply kernel's own drift: a few fewer instructions is no gadget win without a layout control. |
| Let the x86-64 gadgets' RCP, RSQRT and ROUND stop testing once precision is set | Accepted. An estimate whose MXCSR lacks precision reads FPSR (about 3 ns) to decide whether its quotient may raise it; it now adds FPSR's precision to MXCSR when found. Probes improved, Geekbench 6 HDR did not. Copy a sticky flag costly to read to where the cheap test looks. |

## Advertised arm64 extensions

An extension that both engines could execute was kept only if a workload that
uses it ran faster with it advertised and no other workload ran slower, over 16
or more counterbalanced launches, with a private build withholding the hwcap
and ID-register fields so both arms ran identical code.

| Attempt | Disposition and lesson |
| --- | --- |
| Advertise FEAT_JSCVT wherever the host has it | Accepted. V8 lowers ECMAScript ToInt32 to FJCVTZS when the hwcap is set; withholding it made a ToInt32 loop 30 percent to twice as slow. The M1's flushed-subnormal Z flag, the reason for withholding, was already corrected by both engines. |
| Advertise FEAT_MOPS on every host | Accepted. glibc 2.41 selects its MOPS memcpy, memmove and memset from the hwcap, and withholding it made large copies several times slower. The JIT's first version ended the block at every MOPS sequence (a 16-byte memcpy five times slower); page-contained transfers now run inline. A string primitive needs an inline path before it is advertised. |
| Advertise the SME1 profile (FEAT_SME, SME_F64F64, SME_I16I64) wherever the host has it, at any streaming vector length | Accepted, on the M4 with both engines `complete`. Geekbench 6 selects an SME GEMM for machine-learning workloads from HWCAP2_SME alone and they ran 1.1 to 1.75 times faster, from FMOPA/SMOPA outer products no emulated NEON sequence approaches. |
| Add FEAT_SME2 to the advertised profile | Rejected; the profile stays SME1. SME2 drivers lost or tied against SME1: they feed the same outer products and their four-register loads, stores and ZA moves cost more than they save. Under emulation, an extension that only reorganizes data movement gains nothing. |
| Advertise FEAT_CSSC on every host | Accepted. A popcount/ctz/abs/min-max kernel built for CSSC ran faster on both engines (the JIT runs CSSC as short base sequences). Advertising it only tells software that checks the hwcap, such as FEX, that the instructions exist. |

## x86-64 JIT memory order

| Attempt | Disposition and lesson |
| --- | --- |
| Run x86-64 JIT code without x86's store order until the guest kernel marks the mm shared | Accepted. FEXCore's TSO emulation (acquire loads, release stores, barriers around vector accesses) had been on for every address space; keyed on the kernel's sticky mark, single-threaded processes run plain loads and stores, 15 to 37 percent faster on memory-bound work. A thread count does not tell which path a process runs: Geekbench 6's worker briefly maps `/dev/mem` shared and the mark stayed. |
| Clear the mm-shared mark when its last sharer goes | Accepted. The mark follows the mm's live state, so a briefly shared process returns to plain loads and stores; the kernels now describe the machine in SMBIOS tables and have no `/dev/mem`. The per-entry work cost the arm64 kernels about 4 ns per guest entry, so it is built only into the x86_64 slice. |
| Leave accesses through RSP plain in ordered code, as FEXCore does by default | Rejected as unsound; ordered code orders them. Message passing through a variable in a thread's own frame showed the forbidden outcome with the exemption and never without it. |

## AArch32 gadget engine

Measured on the iPad's A15 (gadget engine) with Geekbench 5 armv7 workloads
against the arm named in each row; ratios are throughput ratios, and a
workload not named was level.

| Attempt | Disposition and lesson |
| --- | --- |
| Run VLD1 and VST1 with 16- and 32-bit elements as gadgets | Accepted. They had been left to the semantic library, whose exit the image kernels reached tens of millions of times. A cheap exit is not cheap in a loop; exit counters named the form. |
| Keep a VLD1 or VST1 copy that crosses a page in the gadget | Accepted. Kernels walking image rows crossed pages often enough to exit millions of times; the slow transfer helper moves the bytes after translating both pages, and the library keeps only the misaligned hinted access, whose fault order it owns. |
| Run the parallel add and subtract family and SEL as gadgets | Accepted. Advanced SIMD does the lanes on the low half of a vector register; saturating forms use lane arithmetic because the saturating instructions would set the QC bit the guest reads. |
| Bake the register of an in-place ADD or SUB of an immediate | Accepted, small. An entry per register R0 to R14 with the displacement as an immediate, as the AArch64 bank has, because rename forwards a stored-then-loaded slot only for a base register and a constant displacement. |
| Bake the register of LDR and STR | Rejected. The loaded value's consumers still index CpuState, so only the base forwards and the bank grew: partial baking pays only where both ends of the dependency are baked. |
| Fuse a plain CMP and the conditional branch after it | Accepted. Compares are a tenth of integer instructions and the A64 bank already had the fused pair in a form reading CpuState slots, so the translator rewrites the pair and patches the count behind the branch. |
| Cheaper semantic-library exits | Measured; not pursued. An exit costs about 150 ns, and the large `a32_library` sample share was the FPCR switch routines that follow it in the section, not exits. |
| Remove FPCR switching between VFP and Advanced SIMD floating point (timing-only bound) | Measured. A build whose gadgets never write the FPCR ran image workloads 1.2 to 1.5 times faster, with layout and check controls gaining the same: the switching was the cost. The pairs: float-integer conversions, VMAX/VMIN, and VADD/VSUB/VMUL on scalars against D and Q registers. |
| Run VFP float-to-integer and integer-to-float conversions in either environment | Accepted. A float to integer rounds toward zero in both and differs only for a denormal input; an integer to float differs only when the rounding mode is not nearest. The gadget tests that and switches only then. |
| Run VMAX and VMIN of singles in either environment | Accepted. The result is the same unless an operand is a NaN or a denormal, found with integer arithmetic on lane bits. The corpus runner caught a guard constant kept in V8 (low half kept by the host ABI) and now checks V8 to V15 around every case. |
| Run VADD, VSUB and VMUL of singles in either environment | Accepted. A result of nonzero operands with exponent field 64 to 191 is normal, so with round to nearest and no NaN both environments agree. The test is integer arithmetic on operand bits before the instruction, since testing the result would leave flags behind. |
| Give a VLD1 or VST1 copy without an alignment hint its own gadgets | Accepted. The general structure gadget serves hinted and unhinted accesses and all three writebacks, so the 16-byte copy that is 29 percent of Gaussian Blur's samples ran a dozen unneeded tests; trim a kernel's hottest gadget. |

## JIT semantic-helper exits

A JIT memory word that falls back to its semantic helper switches the FP
environment to the host's and back and ends its block, about 30 ns in all;
GB5 Ray Tracing spent over a third of its vCPU samples in those switches at
4 KiB.

| Attempt | Disposition and lesson |
| --- | --- |
| Prove a memory run's page from the bytes it accesses | Accepted. A run through one base proved and translated the base itself, so a base near a page end whose fields lay further on failed on every execution and ran in the helper. A proof keyed on a stable address fails every time: prove the bytes accessed. |
| Run a memory run that straddles two pages natively | Accepted. A cold path translates both pages and replays the run's accesses on their own host bases; only an access that itself crosses takes the helper. |
| Run LDAR and LDAPR as host load-acquires | Accepted. Every LDAR ended its block in the ordered helper, and C++ reaches LDARB at every function-local static guard; an aligned load now uses the host's own instruction. |
| Keep the FP environment live across helper exits | Rejected before building. The remaining switches are real page crossings, under 1 percent of any workload, against a change to environment code the gadget engine shares. |
| Share the semantic load/store handlers' transfer block in one helper | Rejected twice. `load_store_unsigned` and `load_store_other` end in the same 30 lines, but a shared helper changed both handlers' lowering and measured slower even against a layout-only pad. A pad reproduces a layout shift, not a change inside the handler. |

## JIT top-byte ignore

Android tags heap pointers, so an Android guest always runs with top-byte
ignore, and the JIT tested the setting at run time in every memory word and
left for its semantic helper whenever it was on. That exit cost about 69 ns
per access: a sort on an arm64 Linux guest took twenty times longer with the
setting on, and the Android guest booted several times slower on the JIT than
on the gadget engine.

| Attempt | Disposition and lesson |
| --- | --- |
| Put the setting in the translation key and strip at compile time | Accepted. The setting is constant over a guest entry, so it joins a block's key (bit 61 of an A64 key) and the compiler emits one `ubfx` or nothing; a guest with the setting off also got faster. A link checkpoint storing the key as the next PC must mask the mode bit. |
| Test the setting inline and strip with one branch or a branchless mask | Rejected. Both removed the exit but cost a guest with the setting off 5 to 12 percent on memory-dense probes: only a decision made at compile time takes the test off every access's path. |
| What remains of the Android gap | Not chased. The JIT guest's remaining cost is translating cold code once (publishing blocks, finding identical blocks in the shared cache, link and dispatch tables), not memory exits. |

## JIT cold code per process

LTP's `zram01`, which runs one `dd` per KiB it writes, exceeded its 600 s bound
on the arm64 JIT and took about a third of that on the gadget engine. An exec
costs the JIT about four times what it costs the gadgets, steadily and with or
without ASLR: each exec'd process compiles about 1,760 blocks of ld.so, libc and
the program, with no storm, fallback or retry loop behind it
([Cold code](../darwin/cpu/arm64/jit/README.md#cold-code)). The x86-64 JIT
pays about 2.7 times. The LTP runner scales its bounds by engine instead
([testing](testing.md#ltp)).

| Attempt | Disposition and lesson |
| --- | --- |
| Cheapen the arm64 JIT's cold path: reuse compiler buffers, shrink blocks, keep the literal pool out of the invalidated range | Not pursued: the arm64 JIT runs only on macOS, and an optimization that helps only macOS does not stay. It could not close a fourfold gap; code generation and instruction-cache maintenance bound a block's cost from below. |
| Share position-independent translations across processes, keyed by the code's backing page | Not pursued, for the same reason. It is the only reuse that survives exec and ASLR, since the private cache is keyed by the address space and the shared tier by guest PC. |
| Start each arm64 address space on the gadget engine, as x86's `--jit-tier` does | Not pursued, for the same reason. Exclusives on memory that a gadget process and a JIT process share would need a monitor both engines honour. |

## AArch32 JIT: floating-point environment

The JIT lowers AArch32 in `cpu/arm64/jit/a32`. The work was ordered by the
dynamic host instructions per guest instruction of each workload, by the
share of executor returns and by the busy samples in the host FPCR writes.
Rows are paired comparisons of the single-core workloads against the score
before the change, unless a row says otherwise. The campaign doubled the armv7
scores of the start with the aarch64 score unchanged; AES-XTS and Face
Detection stayed the lowest workloads, then Camera, Horizon Detection,
Machine Learning and Structure from Motion.

| Attempt | Disposition and lesson |
| --- | --- |
| Keep the host FPCR mode across exits and links | Accepted. A Neon instruction runs under the Standard FPSCR value and a VFP one under the guest's; a translation switched at its first Neon instruction and restored at every exit, so Neon loops wrote the FPCR twice per iteration. `CpuState::a32_fp_standard`, shared with the gadget engine, records the mode and a translation writes the FPCR only when it differs. |
| Run VMAX and VMIN under the guest's FPCR behind an integer test | Accepted. The two modes part only on a nonzero denormal, a NaN or a result tiny before rounding, which thirteen integer vector instructions find; a clamp of VMAX/VMIN between two VFP operations had caused 90% of the switches. |
| Run Neon VADD, VSUB, VMUL, VMLA and VMLS the same way | Accepted, with a loss the Standard-FPCR row repaired: pure-Neon loops paid about thirty-five guard instructions per operation where one FPCR write per lap was cheaper. An inexactly underflowing multiplication cannot be taken back, so its operand exponents are tested first. |
| Set the Standard FPCR once, in the preheader, for a Neon floating-point loop | Accepted, repairing the loss above. A guard is for code where the mode is not known; in a Neon loop the mode is known and the guard is the whole cost. A VFP instruction or FPSCR write in the body keeps the guards. |
| Keep the native FP environment across non-FP helper calls | Accepted. A terminal helper restored the host FPCR and FPSR before it ran and the next block activated the guest's again; a memory transfer or integer arithmetic has nothing to do with floating point. |
| Pass VCMP's flags to the next VMRS APSR_nzcv in the host NZCV | Accepted, small. A small effect needs repetitions to match its size: four had shown nothing, ten showed every one at least level. |

## AArch32 JIT: library exits and instruction selection

An instruction the compiler sends to the semantic library ends its block: a
decode, an execute, a return to the executor and a re-entry.

| Attempt | Disposition and lesson |
| --- | --- |
| Inline loads with register-offset writeback whose destination is the offset | Accepted. The form was 99% of Horizon Detection's library exits; the load goes to a scratch register and the destination is written last, as the pseudocode orders it. Count exits by form before choosing what to lower. |
| Lower the parallel add and subtract forms to the vector unit | Accepted. UADD8 and UQSUB8 (glibc's string kernels) were 91% of Speech Recognition's library exits. Nothing may set the host's QC flag (the guest's FPSCR.QC), so unsigned saturating forms use UMAX/UMIN and GE comes from a widening add. Re-measure at the tip once the larger bottleneck is gone. |
| Run a conditional data-processing instruction on both arms and select with CSEL | Accepted. A comparison selecting one of two values is a data-dependent branch over a short body, which mispredicts; hardware ARM executes it either way. Flag-setting forms, register-controlled shifts, loads, stores and the PC keep the branch. |
| Keep a page-crossing Advanced SIMD load in the translation | Accepted. A cold path probes both pages and reads through the host mapping when they are adjacent, or from a 64-byte staging copy; stores still take the library, which orders them. |
| Fuse an exclusive run that leaves on a conditional branch | Accepted, targeted. The compare-and-swap that gives up on a mismatch and the try-lock did not fuse, so every pass took the helper twice; few workloads gain. |

## AArch32 JIT: memory access

Inline TLB probes are 20% to 40% of the host instructions executed in the
memory-heavy single-core workloads, so a shorter probe is a general saving.

| Attempt | Disposition and lesson |
| --- | --- |
| Probe the 4 KiB TLB in two fewer instructions, skip the crossing test of an access aligned to its size, and fold the crossing test into the tag compare | Accepted as three changes with one measurement. Comparing the tag with the page of the last byte makes the crossing test free; an aligned access cannot cross. Tests compare emitted words, because a probe with a wrong index passes behavioral tests by missing. |
| Keep the exclusive monitor across ordinary stores (A64 and A32) | Accepted as a correctness fix. Clearing the monitor at every store, as the architecture permits, failed a LDREX/STREX pair on every retry when a spill sat between them (ART's card-table update), and the 32-bit zygote's collector spun forever. The JIT now clears only where a pair ends and relies on reservation versions. |
| Add the TLB delta in the access, as `[x9, w8, uxtw]`, instead of before it | Accepted. The add was a few percent of executed host instructions and most accesses after it have the register-offset form; pairs, multiples, acquire/release, lane transfers and stores that record their host range keep the add. |

## AArch32 JIT: blocks, links and registers

| Attempt | Disposition and lesson |
| --- | --- |
| Go on past a conditional forward branch (B, CBZ, CBNZ) in a block, with a cold side exit for the taken path | Accepted. Every conditional branch had ended a block with two links, so the short blocks of the benchmarks paid a link sequence and the loads of the next block on every transition. |
| Run a block that branches back to itself as a host loop | Accepted. A probe compilation records the registers the body holds, the loop loads them once and the back edge publishes only the flags and checks the budget. |
| Hold the loop's most used registers, not its first | Superseded. Neutral alone and made moot when a translation took all fifteen core registers. |
| Keep the target of a forward branch in the block, over the instructions it skips | Rejected and reverted. It carried the most compiler state (second bindings and flag states, a join, a retry) for a cold side exit that costs one link, and made reachable a latent defect: a cold path stores dirty registers through the bindings at block end, wrong once a binding is forgotten mid-block. |
| State the loaded FP state and the native environment in every chain's link selector | Accepted. A chain's selector described its first block, so a loop of an integer block and a Neon block returned to the executor at both edges of every iteration. |
| Look up an indirect branch's target in a per-executor table behind its two cells | Accepted. A return from a function with more than two callers cannot keep its targets in two cells; a 4096-entry direct-mapped table of trained targets, tagged with the link epoch because it outlives address spaces (untagged, it followed another address space's function and killed init). |
| Hold all fifteen core registers in the host (x22 to x28 beside the eight) | Accepted, small on its own, and the precondition of the next row: a block that names r0 to r12 did 25 register-file accesses per pass with eight host registers. |
| Keep the core registers in the host across links | Accepted. A linked translation loads the fifteen registers once before the internal entry only the executor reaches; a return to the executor writes them back. A kept register set turns every hard-coded scratch register into one, so tests write r0 to r2 first. |
| Go on past a conditional branch back | Accepted. A loop body testing its bounds with several branches back to the increment was a chain of tiny blocks that fall through nearly always, each paying a link; a branch back to the block's own first instruction still ends it, for the host loop. |

## AArch32 JIT: candidates not built

| Attempt | Disposition and lesson |
| --- | --- |
| Cache the last page ahead of the TLB probe | Rejected before building. The crossing test an access needs beside the cached page compare costs what the cache saves. |
| Keep the vector registers in the host across links as the core registers are | Not built. Vector spills and fills are a few percent of executed host instructions, for a fixed binding of the A64 compiler's dynamic vector residency and every helper that reads the vectors. |
| Fold the alignment test into the tag compare | Not built. The alignment test is a small share of the executed host instructions, not a gain a paired run resolves. |

## AArch32: AES-XTS and Face Detection

With the JIT well behind aarch64 single-core, AES-XTS and Face Detection were
the lowest Geekbench 5 armv7 workloads. Each JIT row compares concurrent pairs
of 1-vCPU guests against the commit before it; the aarch64 emission is
unchanged.

| Attempt | Disposition and lesson |
| --- | --- |
| AES-XTS in either engine | Intrinsic; nothing changed. The armv7 binary holds no AES instructions although the guest advertises the crypto extension; Geekbench encrypts with tables, an order of magnitude more guest instructions than the aarch64 build. Count instructions per unit of work of the two binaries before looking for an engine cause. |
| Face Detection in the gadget engine | Nothing specific to armv7. The gadget engine addresses each S register in CpuState, so the lane inserts of the JIT rows below do not arise there. |
| Convert singles to and from integers in the vector unit (JIT) | Accepted. VCVT between a single and a 32-bit integer went through W0 twice; the A64 scalar SIMD forms round, saturate and raise flags like the general-register forms. The corpus's simd-convert suite now has fractional inputs, which could not tell FCVTZS from FCVTNS. |
| Keep S registers in V registers through runs of VFP instructions (JIT) | Accepted. An S register is a lane of its resident Q register, so every single-precision instruction inserted its result, an insert rewrites the whole Q, and independent chains serialized. In a run of unconditional single-precision instructions an S register keeps a copy in a low V register that later reads take. |
| Let an A32 block hold all 64 instructions (JIT) | Accepted. The 60-instruction target leaves room for an IT block that a block's last instruction opens, which only T32 has; one block of 64 saves a link. |
| Link a loop block's exit through the unconditional B after it (JIT) | Not built. The change reaches into the host loop's exits, budget and saved guest words for about a link per pass. |
| Cache the TLB page for a run of table loads | Not built. A translation holds all fifteen core registers, which leaves no host register for a cached page, and the last-page cache was rejected for the crossing test it needs. |

## AArch32 gadgets against the Raspberry Pi 4

Each row runs Geekbench 5 armv7 single-core in the gadget engine against the
commit before it. The engine's gap is its armv7/aarch64 ratio divided by the
same ratio on a native Raspberry Pi 4 (Cortex-A72); the accepted rows narrowed
it slightly.

| ID | Attempt | Disposition and lesson |
| --- | --- | --- |
| G242 | AES-XTS against the Pi 4 | Measured; nothing to change. The Pi 4's Cortex-A72 has no AES instructions, so both its builds use tables while our aarch64 guest runs host AES: the gap measures that, not a slow armv7 path. The armv7 rounds are mostly table loads, which G243 shortens. |
| G243 | Key the 4 KiB AArch32 TLB probe by the access's last byte | Accepted. The folded 4 KiB index puts pages n and n + 1 in one set only when n has 25 trailing ones, which no 32-bit page number has, so the crossing test joins the tag compare (9 probe instructions instead of 12). A wrong set index shows only as misses the C++ fill path hides, so the control test must hit through the emitted probe. |
| G244 | Run conditional data processing that sets no flags as a select | Accepted. A conditional instruction went behind a skip gadget whose branch follows the guest's condition, which the host mispredicts; MOV, MVN, ADD, SUB, RSB, AND, ORR, EOR, BIC and MOVW now select by a truth table over NZCV. A branch on guest data costs more than the work it skips. |
| G245 | Run the ADD or SUB that computes a VLD1 or VST1 copy's base inside the copy | Accepted. The gadget of a VLD1/VST1 copy does the ADD that computes its base and writes the register before the transfer. Only unconditional Advanced SIMD data processing that reads no core register may sit between, which cannot exit or fault. |
| G246 | Give hinted VLD1 and VST1 copies and VLD1 to all lanes their own gadgets | Accepted. Machine Learning's hinted copies and loads to all lanes ran through the general structure gadget and its dozen tests; the new gadgets take the alignment mask in their operand word and leave a misaligned access to the library, which owns fault order. |
| G247 | Fuse VCMP and the VMRS APSR_nzcv after it | Accepted. Ray Tracing and Speech Recognition compare floats and move the flags to the core at once; the compare's gadget is rewritten to a fused one that sets FPSCR's flags and NZCV in one dispatch. |
| G248 | Run a conditional VMOV between VFP registers as a select | Accepted, small. A conditional VMOV of D registers went behind a skip like any other conditional instruction; FCSEL copies the bits without raising anything, so the select needs no environment. |
| G249 | Let an A32 block hold all 64 instructions | Accepted, small. The translator stopped every block five instructions short of its window so a started IT block could complete, which only T32 needs; an A32 block holds 64, a bound (`a32::block_limit`) that gadget words carrying a position may rely on. |
| G250 | Fuse the dependent VMUL and VADD chains of Gaussian Blur and Machine Learning | Not built. The quad-register VMUL and VADD gadgets are over a third of Gaussian Blur's samples and each reloads the register the one before stored; a pair gadget saves a dispatch and a reload but needs one per pairing of operations and shapes. |

## AArch32 gadget stream size

Each row shrinks what the AArch32 gadget builder stores per instruction. Stream
words come from a static sweep that translates Geekbench 5 armv7, the Debian
armhf release's /usr and a set of armeabi-v7a apps without running them; cache
memory from a census of Geekbench 5 armv7 single-core at 131,072 slots with
eight ways. Together the accepted rows took about a fifth of the A32 and T32
stream words with Geekbench 5 single-core level to slightly faster, and leave
the AArch64 gadget parts byte-identical.

| ID | Attempt | Disposition and lesson |
| --- | --- | --- |
| G251 | Fold a PC register operand into the immediate form | Accepted. PC as an unshifted register operand is a constant, so the builder emits the immediate or offset form. A fold whose other operand is its destination takes the indexed immediate entry, not G196's in-place one, since indexed loads consume the sum: a fold that reaches a specialised entry inherits its assumptions. |
| G252 | Take a linked call's LR from its return link | Accepted. A BL or BLX with a linked return follows that link, whose key is the return address with the instruction set in bit 62, so the linked and cached call gadgets derive LR from it and the builder drops the LR word. |
| G253 | Let the memory transfers carry a position instead of a key | Accepted. Single and doubleword transfers, VLDR, VSTR, TBB and TBH pushed their key only so a fault could name the instruction; they carry their block position in the operand word and the executor resolves it. A word kept only for a fault costs every translation. |
| G254 | Fold a MOVT into the MOVW before it | Accepted, small. A MOVT right after an unconditional MOVW of the same register outside an IT block makes the pair one move of the whole constant; the pair is rare in code that loads constants from literal pools. |
| G255 | Recompute a block transfer's library operand instead of storing it | Not built. LDM/STM's library operand for the page-crossing path is under one percent of the stream, and rebuilding it means finding the block's body or fetching the guest word by a key the gadget no longer carries, coupling the library to the program cache. |
| G259 | Let the block transfers carry a position instead of a key | Accepted. LDM, STM, PUSH and POP carry their block position as G253's transfers do, and the library runs the instruction at address 0 since no block transfer the gadgets run reads its own address (an STM storing the PC goes to the library with its key). A one-workload loss spread over untouched gadgets was layout: a never-run pad reproduced it. |
| G260 | Pack a block transfer's registers five bits each | Accepted. Registers were CpuState byte offsets, eight to a stream word; each is now its offset over four in five bits, so six fit beside the base and position and every block transfer carries a word less. |
| G261 | Hold a short skip's distance in the gadget | Accepted. Each condition also has a skip for every run of two to nine words with the distance as an immediate (112 gadgets), covering almost all skips. A builder removing a word from a stream under construction must move every index it recorded past it. |

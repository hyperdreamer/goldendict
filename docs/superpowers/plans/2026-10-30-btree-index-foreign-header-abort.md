# Btree Index Foreign-Header Abort Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use the deterministic
> subagent-driven-development controller to implement this plan task-by-task.
> Steps use checkbox (`- [ ]`) syntax for readability; do not tick them during execution because the controller pins this document's digest.

**Goal:** Reject and rebuild foreign/corrupt persisted trees while containing every index-controlled search, article, resource, and FTS failure on its request, without rejecting valid writer output.

**Architecture:** Keep the existing native-format indexes and codecs. Put state validation, transactional node decoding, and per-traversal cycle detection in `BtreeIndex`; harden the independent storage decoders and their actual consumers; extend every owning producer's existing rebuild guard. One production-linked, GUI-less QTest suite and isolated mutation/acceptance drivers prove the contracts rather than reproducing validators in tests.

**Tech Stack:** C++17, Qt 6/QTest/QCoreApplication, qmake6, zlib, existing ZIP/MDict/optional LZO/ZIM/EPWING dependencies, Python 3 acceptance drivers, Arch compiler/assertion flags.

## Global Constraints

- Authority: read `docs/superpowers/specs/2026-10-30-btree-index-foreign-header-abort-spec.md` in full. Its §10 compatibility corrections override conflicting prose in `docs/superpowers/specs/2026-10-30-btree-index-foreign-header-abort-design.md` (v5, approved at `4fd12d447b096adee93c51df70230b9693b6866c`). All line ranges in this plan are pre-change navigation anchors, not fixed patch coordinates.
- No on-disk format or format-version change: preserve every header size, field width/order, native index endianness, ZIP endianness, compression wrapper, filename convention, and version expression. `BtreeIndexing::FormatVersion = 4`; reuse the single definitions `BtreeMinElements = 64`, `BtreeMaxElements = 8192`. Do not change packaging/release versions or `loaddictionaries.cc`.
- Do not invent constants as correctness bounds. In particular no fixed node/resource allocation ceiling and no traversal-count ceiling. The deflate bound is exactly `uint64_t(C) * 1032u + 64u`; multiplication widens before execution. Windows of at most 65,536 output bytes are allocation granularity, not an output limit. Do not apply deflate properties to plain/LZO blocks.
- Preserve valid large chains (including the 1,500,000-link builder fixture), 65,537-byte and larger blocks/resources, empty roots, allocated empty chunks with a real zlib stream, empty resource contents, one-past-end zero-length block starts, and scans across 4,100 distinct leaves. `{U=0,C=0}` is not a compressed stream. Nodes, unlike chunks, require nonzero U.
- Validate file bounds/count products/representability before allocation, pointer formation, indexing, scanning, or copying. Use uint64 arithmetic, subtraction after range proof, signed-size checks before conversion, unaligned-safe `memcpy`, and explicit lengths/bounded terminators. A zero-byte operation must skip `front()`, empty-buffer pointer arithmetic, and copying entirely.
- Preserve the public catching/logging `findArticles()` API; request owners use `findArticlesChecked()` to distinguish corruption from a miss. Never-opened throws `exIndexWasNotOpened`; attempted bad open returns normally and subsequent checked traversal throws `exIndexInfoUnusable`. No last-error global/thread-local, worker rebuild, process termination, or unrelated file mutex.
- Lane ownership is exclusive: `lane-core` owns `btreeidx.hh`, `btreeidx.cc`; `lane-storage` owns `chunkedstorage.hh`, `chunkedstorage.cc`, `indexedzip.hh`, `indexedzip.cc`; `lane-producers` owns `sounddir.cc`, `stardict.cc`, `dsl.cc`, `gls.cc`, `xdxf.cc`, `sdict.cc`, `aard.cc`, `bgl.cc`, `dictdfiles.cc`, `lsa.cc`, `zipsounds.cc`, `epwing.cc`, `zim.cc`, `slob.cc`, `mdx.cc`, `ftshelpers.cc`, `mdictparser.cc`; `lane-tests` owns `tests/**`. Each task may commit only its own listed subset. Do not add production helper files outside these allowlists.
- Core tasks come first to establish the predicate/checked lookup/protected helper interfaces. To reconcile this with spec §11.1's test-first order and exclusive `tests/**` ownership, every production task FIRST writes and runs QTest probes in a disposable source copy, changes only its owned production files, then overlays the production diff into a fresh copy and reruns those probes. Tasks 5–7, 11, 16, and 23 promote the corresponding fixtures/slots into the committed suite. Disposable probes are not a substitute for those final slots or the aggregate. This is the only scheduling adaptation to spec §11; no acceptance requirement is deferred out of this plan.
- In each production task, copy sources with `cp -a "$GD_SRC/." "$GD_PROBE_SRC/"` into a newly created absolute temporary directory outside `$GD_SRC`; remove the copied `.git` directory/file before editing. Before Task 5, create the QCoreApplication `.pro`/fixture probe described in Task 5 only in that copy. Later, overlay new probe slots/fixtures onto its existing suite. Preserve RED/GREEN logs and the fixture source in the PM reports, but never stage disposable tests from a production lane. A baseline intentional abort/cycle runs only in a bounded subprocess, not the aggregate process.
- Set `GD_SRC=$(git rev-parse --show-toplevel)`, `GD_BUILD_ROOT=$(mktemp -d /tmp/gd-btree-build.XXXXXX)`, and `PM_RUN_ROOT` to the existing absolute run root for `pm-run-20261030-030606-0654b80a`. Reports go to `$PM_RUN_ROOT/reports/`, never to a new repository `reports/`. Keep source identity, commands, configuration, exit status, assertion/sanitizer diagnostics, and selected slots in each gate record.
- All build output is outside the checkout. For an isolated suite build use the function below (spec §11.1, with a unique build root); `GD_SRC` may point to a disposable probe/mutant copy. Never run `qmake6 tests/tests.pro -o tests/Makefile` from the active checkout. The aggregate equivalent is an absolute source `.pro` and an external output Makefile.

```bash
step() { # step <unique-name> <QTest-slot>...
  local d="$GD_BUILD_ROOT/$1"; shift
  test ! -e "$d" || return 1
  mkdir -p "$d" || return 1
  qmake6 "$GD_SRC/tests/tst_btree_index/tst_btree_index.pro" -o "$d/Makefile" || return 1
  make -C "$d" -j"$(nproc)" || return 1
  QT_QPA_PLATFORM=xcb xvfb-run -a "$d/tst_btree_index" "$@"
}
```

- Every QCore suite invocation arms `TestSupport::installWatchdog(TestSupport::PureWatchdogSeconds)` (60 seconds), uses `requiredEnvironment`, and checks normal exit status for children. Opening/cycle children have a 5-second wall deadline; kill/wait cleanup is mandatory after timeout and timeout/crash is failure. The existing aggregate outer timeout remains 900 seconds. Do not disable `_GLIBCXX_ASSERTIONS`; ordinary and ASan/UBSan objects never share output directories.
- All test forwards use `GD_BTREE_INDEX_TEST`, are compiled in the real owner translation unit, and call the actual guard/reader/worker. No parallel parser, copied guard, silent QSKIP for a required feature, or uncalled production source counts as coverage. Private cache observations use a narrow test-only friend, not wholesale protected access.
- Shared planned test files: `tests/tst_btree_index/{tst_btree_index.pro,tst_btree_index.cc,fixtures.hh,fixtures.cc,owner_adapters.hh}`. Keep new `.cc` files here, not in `tests/support/`: that directory's glob links into every suite. Fixture support names are `BtreeTest::TreeFixture`, `TestIndex`, `W32`, `link`, `record`, `buildWords`, and `runChild`; their contracts/code are specified in Task 5. Test-only `BtreeIndexing::BtreeIndexTestAccess` observes private state. Producer guard forwards are named `<Owner>::btreeTestOldOrBad` and retain the real guard's argument types/order; `FtsHelpers::ftsIndexIsOldOrBad` is already public.
- Worker test forwarding contract, declared in `owner_adapters.hh` in Task 5 and included by production files only under `GD_BTREE_INDEX_TEST`: `BtreeTest::WorkerResult <Owner>::btreeTestWorker(BtreeTest::WorkerCase const &)`. `WorkerCase` contains `enum Route { Article, Resource, ArticleText, Metadata, Reader, Prefix, Fts } route`, `std::string indexFile`, `std::vector<std::string> dictionaryFiles`, `gd::wstring word`, `std::string resource`, `uint32_t address`, and `QString variant` (fixture selector for the actual production mode/reader, not a separate implementation). `WorkerResult` contains `bool success`, `QString error`, `QByteArray data`, `QString text`, `int finishedSignals`, `bool runnableExited`. A forward constructs/injects the real dictionary/reader using these paths and invokes the actual route; it observes the real request's error, data, finished signal and runnable retirement. Set up observation before scheduling via a narrow test-only scheduling hook where necessary; never fabricate completion counts or implement decoding in a forward. Low-level reader exceptions propagate for exception assertions; actual request routes must return their real error channel. Already-finished instant requests are inspected directly; their pre-return emission cannot be observed by a spy attached after return and is not a fabricated asynchronous count.
- Census types, also defined in Task 5's `owner_adapters.hh`: `BtreeTest::PairObservation { QString role, owningFile, maxField, rootField; uint32_t m=0, root=0; uint64_t fileSize=0; bool present=false, sane=false; }` and `BtreeTest::PairReport { bool oldOrBad=true; QString error; std::vector<PairObservation> pairs; }`. A production census forward reads its real bounded header/table reader and records actual fields and predicate results; it does not implement an alternative parser. Absence is separate from an invalid present pair. Failed header reads remain an error report, never an invented zero pair or a dropped file.
- Local acceptance is not a 51st hermetic slot. Tasks 25–27 must build the real application, hash the read-only corpus, exercise an isolated scratch **HOME override** (`HOME=<tmpdir>` plus XDG_* under it), and perform the final format diff audit. Portable mode is explicitly excluded: `main.cc:477` clears `cfg.soundDirs` and `sources.cc:135-147` disables them, so a portable launch cannot load SoundDir at all. This is resolved by spec correction C7 (`docs/superpowers/specs/2026-10-30-btree-index-foreign-header-abort-spec.md` §8.4, §10) — the scratch-HOME route is the approved gate, and no portable-mode SoundDir expectation is required. Never write into the user's real home/index from a gate.
- Same-lane tasks are sequential by explicit dependencies. Cross-lane dependencies name actual consumed APIs, storage behavior, or fixture/adapter contracts. Do not edit this approved plan while executing. If a final gate exposes a defect in another lane, return it to that owner through the controller's repair/review mechanism, not an out-of-allowlist edit.

## Task 1: Establish checked lookup, predicate, and inert-open state contracts

**Implementer tier:** Frontier
**Lane:** lane-core
**Owns:** btreeidx.hh, btreeidx.cc
**Depends on:** none

**Files:**
- Modify: `btreeidx.hh:35-157` (exception, constants, state, declarations, narrow observation friend).
- Modify: `btreeidx.cc:44-122`, `514-545`, `855-866`, `1263-1307`, `1406-1433` (constructor, opening, checked lookup, entry guards).
- Test, disposable first: T01 `builds_and_reads_back`, T02 `open_index_terminates`, T04 `insane_index_info_is_inert`, T23 `reopen_valid_then_invalid_then_valid`, T24 `default_constructed_is_unopened`, T25 `every_entry_point_guarded_after_invalid_open`, T26 `shared_file_position_restored_and_no_deadlock`. Committed slots: Task 5.

**Interfaces:**
- Consumes existing `IndexInfo(uint32_t,uint32_t)`, `File::Class::{tell,seek,file}`, recursive `Mutex::Lock`, `readNode(uint32_t,vector<char>&)`; the decoder is replaced in Tasks 2–3, not duplicated in the predicate.
- Produces public `static bool BtreeIndex::indexInfoIsSane(IndexInfo const&, File::Class&, Mutex&)`, unchanged `void openIndex(IndexInfo const&,File::Class&,Mutex&)`, and `vector<WordArticleLink> findArticlesChecked(wstring const&,bool ignoreDiacritics=false)`.
- Declares protected `TraversalState { std::set<uint32_t> visitedOffsets; }`, `static void enterNode(TraversalState&,uint32_t)`, `readNextLeaf(uint32_t&,vector<char>&,char const*&,TraversalState&)`, and the existing protected lookup plus final `TraversalState * traversal = 0`. Definitions/call-site wiring of the new traversal helper are Task 4; existing lookup definition accepts the added parameter now. Keep metadata/cache private; add `bool indexUsable = false` privately.

- [ ] **Step 1: Write the failing state/open probes in the disposable fixture harness**

Use production builder trees A (`a`, offset 7), B (`b`, offset 9), and invalid `IndexInfo(0,0)`. The state tests must contain these assertions, plus the full entry matrix and warning observation:

```cpp
QVERIFY_EXCEPTION_THROWN(index.findArticlesChecked(Utf8::decode("a")), exIndexWasNotOpened);
index.openIndex(infoA, fileA, mutexA);
QCOMPARE(index.findArticlesChecked(Utf8::decode("a")).at(0).articleOffset, uint32_t(7));
index.openIndex(IndexInfo(0, 0), fileA, mutexA); // returns, does not throw
QVERIFY_EXCEPTION_THROWN(index.findArticlesChecked(Utf8::decode("a")), exIndexInfoUnusable);
QVERIFY(index.findArticles(Utf8::decode("a")).empty());
index.openIndex(infoB, fileB, mutexB);
QVERIFY(index.findArticlesChecked(Utf8::decode("a")).empty());
QCOMPARE(index.findArticlesChecked(Utf8::decode("b")).at(0).articleOffset, uint32_t(9));
```

T02 runs sane open/query in a child. In T25 pass sentinel pointer/output arguments to protected `readChain`/lookup; prove the guard runs before touching them. Test all seven original entries; new helper entry is added in Task 4. Test position preservation on a valid root and rejected fan-out while holding the recursive mutex; the full malformed-root/concurrent matrix is Task 5 after the safe decoder lands.

- [ ] **Step 2: Run RED before production edits**

Build/run the disposable probe out of tree with the `step` recipe. Missing new declarations are the initial API RED; after compiling the declarations prove the old invalid-open/cache behavior fails an assertion in its isolated process. Preserve the exact diagnostic. Do not run a zero-U valid-fan-out root in the main test process before Task 2.

- [ ] **Step 3: Implement reset/validate/commit and checked lookup**

Move, do not duplicate, the two unchanged builder constants into the namespace header. Add exactly `DEF_EX( exIndexInfoUnusable, "The index header does not describe this file", Dictionary::Ex )`. Initialize both pointers, both numeric members and root-cache state. Guard every original entry before state/query/pointer access with the following ordered checks (inside the public wrapper's try):

```cpp
if ( !idxFile ) throw exIndexWasNotOpened();
if ( !indexUsable ) throw exIndexInfoUnusable();
```

Move the successful lookup/antialias body into `findArticlesChecked`; the public wrapper calls it in its existing logging catches and clears partial results. The predicate's body is a single supplied-mutex save/read/restore window:

```cpp
try {
  Mutex::Lock lock(mutex);
  qint64 saved = file.tell();
  if (saved < 0) return false;
  bool sane = false;
  try {
    qint64 signedSize = file.file().size(); // flush buffered writes first
    if (signedSize >= 0 && info.btreeMaxElements >= 1 &&
        info.btreeMaxElements <= BtreeMaxElements) {
      uint64_t S = uint64_t(signedSize), o = uint64_t(info.rootOffset);
      if (o <= S && 8 <= S - o) {
        BtreeIndex probe;
        probe.idxFile = &file; probe.idxFileMutex = &mutex;
        probe.indexNodeSize = info.btreeMaxElements;
        probe.rootOffset = info.rootOffset;
        vector<char> decoded;
        probe.readNode(info.rootOffset, decoded);
        sane = true;
      }
    }
  } catch (...) { sane = false; }
  try { file.seek(saved); } catch (...) { return false; }
  return sane;
} catch (...) { return false; }
```

Never call `openIndex` or a guarded traversal from this predicate. No throwing restoration destructor. `openIndex` first zeroes fan-out/root, unloads/clears cache and clears usability, saves the supplied pointers, defensively catches predicate failures, and commits fan-out/root then usability only on success. It does not steal the probe cache. Task 1 establishes the state/API layer; Tasks 2–3 are mandatory before claiming corrupt-root safety.

- [ ] **Step 4: Run GREEN state/open controls and compile the real request closure**

Rebuild a fresh probe copy with only this task's core diff. Run T01/T02/T04/T23–T26's currently constructible cases. Verify cache-empty observations after both invalid and valid reopen, same/different-file variants, normal child exit and correct warnings. Compile the real `btreeidx.cc` request classes; no test subclass-only access claim.

- [ ] **Step 5: Commit**

```bash
git add btreeidx.hh btreeidx.cc
git commit -m "fix(btree): add checked lookup and inert index opening"
```

## Task 2: Decode node records incrementally with strong publication

**Implementer tier:** Frontier
**Lane:** lane-core
**Owns:** btreeidx.cc
**Depends on:** 1

**Files:**
- Modify: `btreeidx.cc:1-38`, `458-512` (node record decoder and file-local RAII).
- Test, disposable: T05 `zeroed_node_record_throws`, T06 `short_node_record_throws`, T13 `out_of_range_node_offset_throws`, T15 `max_node_ceiling_rejects_before_allocation`, T28 `compressed_record_bounds_use_wide_arithmetic`, T29 `incremental_inflate_checks_stream_and_length`. Committed slots: Task 6.

**Interfaces:**
- Consumes Task 1's initialized probe/private fan-out and `indexInfoIsSane(IndexInfo const&,File::Class&,Mutex&)`.
- Produces protected `void readNode(uint32_t offset,vector<char>& out)` with file-range/stream/length proofs, no usability guard, and `out.swap(decoded)` only on success. Caller holds the file mutex. Task 3 completes the shape stage before publication.

- [ ] **Step 1: Write framed-stream RED probes, reaching the decoder after sane bootstrap opening**

Append `W32(0)+W32(0)`, payloads of actual/advertised 1–3 bytes, incomplete eight-byte headers, C beyond EOF, tiny C with `U=UINT32_MAX`, invalid checksum, truncation, compressed tail and U mismatches. Pass appended offsets directly to `TestIndex::readNode`; prefill output with `{'s'}` and require it unchanged after failure. Include root-at-zero success. Use a sparse `QTemporaryFile::resize` near 32-bit boundaries; document sparse allocation and measure Linux resident-size delta so an output-sized allocation cannot pass unnoticed.

```cpp
vector<char> out(1, 's');
QVERIFY_EXCEPTION_THROWN(f.index.readNode(badOffset, out), exFailedToDecompressNode);
QCOMPARE(out, vector<char>(1, 's'));
```

- [ ] **Step 2: Run RED in bounded child processes**

Use `step record-red zeroed_node_record_throws short_node_record_throws out_of_range_node_offset_throws max_node_ceiling_rejects_before_allocation compressed_record_bounds_use_wide_arithmetic incremental_inflate_checks_stream_and_length` on the disposable probe. Record original assertions/stream acceptance/allocation failure, not a root-opening rejection. Crashed baseline children are evidence of RED, never a passing expected final outcome.

- [ ] **Step 3: Replace the record decoder using the spec's decision order**

Flush through `file()`, reject negative size, prove `o<=S`, `8<=S-o`, read U/C, reject U/C zero, prove `C<=S-o-8`, then compute the wide limit. Check `size_t`, vector max-size, qint64 and zlib input-window representability before narrowing. A file-bounded C-byte input vector is permitted; do not allocate/reserve U bytes. Use a file-local zlib end guard whose destructor only calls `inflateEnd` after successful initialization. Incremental loop core:

```cpp
uint64_t consumed = 0, produced = 0;
std::array<unsigned char, 65536> window;
for (;;) {
  if (zs.avail_in == 0 && consumed < uint64_t(C)) {
    uint64_t take = std::min<uint64_t>(uint64_t(C) - consumed, UINT_MAX);
    zs.next_in = reinterpret_cast<Bytef *>(input.data()) + size_t(consumed);
    zs.avail_in = uInt(take);
  }
  uInt beforeIn = zs.avail_in;
  zs.next_out = window.data(); zs.avail_out = uInt(window.size());
  int rc = inflate(&zs, Z_NO_FLUSH);
  uint64_t used = uint64_t(beforeIn - zs.avail_in);
  uint64_t made = uint64_t(window.size() - zs.avail_out);
  consumed += used;
  if (produced > limit || made > limit - produced ||
      produced > uint64_t(U) || made > uint64_t(U) - produced)
    throw exFailedToDecompressNode();
  if (made > decoded.max_size() - decoded.size()) throw exFailedToDecompressNode();
  if (made) decoded.insert(decoded.end(), window.begin(), window.begin() + size_t(made));
  produced += made;
  if (rc == Z_STREAM_END) {
    if (consumed != uint64_t(C) || produced != uint64_t(U)) throw exFailedToDecompressNode();
    break;
  }
  if (rc != Z_OK || (!used && !made)) throw exFailedToDecompressNode();
}
```

Initialize with `inflateInit` for the existing zlib wrapper; U and output growth must fit all receiving containers. Reject output shorter than four bytes before a first-word read. Translate record seek/read failures to `exFailedToDecompressNode`; allocation exceptions may propagate. Leave file at `o+8+C` on success. Keep decoded local until Task 3's shape checks complete. Comment that disabled `__BTREE_USE_LZO` cannot be enabled without equivalent codec-specific safety/tests; do not apply the deflate bound to LZO.

- [ ] **Step 4: Run GREEN, including the high-ratio positive and saved-position controls**

Run the same slots plus T01/T02/T26. Build the exact 32-MiB leaf from spec §8.2: word length `32*1024*1024-14`, empty prefix, zero offset; assert payload size, `U>C*516+64` and `U<=C*1032+64`, then compare decoded bytes. Ensure no unused input/incorrect U succeeds and no partial output/root-loaded state publishes after failure.

- [ ] **Step 5: Commit**

```bash
git add btreeidx.cc
git commit -m "fix(btree): bound node framing and inflate actual output"
```

## Task 3: Validate complete node and inner-chain shape before publication

**Implementer tier:** Frontier
**Lane:** lane-core
**Owns:** btreeidx.cc
**Depends on:** 2

**Files:**
- Modify: `btreeidx.cc:458-512`, `552-747`, `855-893` (shape stage and unaligned-safe consumption).
- Test, disposable: T07 `truncated_node_offsets_array_throws`, T08 `unterminated_separator_throws`, T09 `absurd_leaf_entry_count_throws`, T10 `leaf_framing_not_exact_throws`, T11 `zero_size_chain_throws`, T12 `truncated_inner_chain_record_throws`, T16 `builder_large_node_reads_back`, T27 `internal_node_consumes_exact_separator_region`, T30 `corrupt_descendant_rejected_after_sane_root`, T31 `empty_root_allowed_empty_child_rejected`. Committed slots: Task 6.

**Interfaces:**
- Consumes Task 2's local decoded vector and protected `readNode(uint32_t,vector<char>&)` record stage.
- Produces that same API with exact internal/leaf/inner-chain framing on every decoded node, including descendants. It accepts exactly four zero bytes as a decoder-level empty leaf; traversal applies context in Task 4.

- [ ] **Step 1: Write the malformed-shape matrix and large-builder positive first**

Follow spec §8.2 framing, not a test validator. Every case appends to an already sane open tree; descendant cases mutate only after root opening. Enumerate short child arrays including one-byte-short; missing/intermediate/final/doubled NUL separators; extra separators/tail; counts `m+1` and `0xfffffffe`; missing chain-size, zero chain, one-byte-past-end chain, trailing bytes, count mismatch; absent word/prefix terminators and offsets short by 1–4. Include the 12-byte leaf `W32(1)+W32(4)+four payload bytes`. A complete empty-word/empty-prefix/four-byte-offset record is a structural positive, not forbidden semantics.

```cpp
QByteArray payload = BtreeTest::W32(1) + BtreeTest::W32(0);
uint32_t o = f.append(BtreeTest::record(payload));
vector<char> out;
QVERIFY_EXCEPTION_THROWN(f.index.readNode(o, out), exFailedToDecompressNode);
```

- [ ] **Step 2: Run RED without letting open rejection satisfy the test**

Run T07–T12/T27/T30 probes and assert the decoder entry counter/source trace reached the appended offset. Run the large-chain positive separately to establish it is readable before adding shape checks. Do not replace it with a hand-framed large record.

- [ ] **Step 3: Add cursor-based exact shape validation and safe reads**

With `n=decoded.size()`, copy the first word only after `n>=4`; m must be 1 through `BtreeMaxElements`. For an internal node prove `4+(uint64_t(m)+1)*4<=n`, then find exactly m non-empty separators by bounded `memchr`, requiring exact final consumption; do not follow children here. For a leaf prove count<=m and implement this loop before swapping:

```cpp
size_t p = 4;
for (uint32_t e = 0; e < first; ++e) {
  if (p > n || 4 > n - p) throw exFailedToDecompressNode();
  uint32_t chainSize; memcpy(&chainSize, decoded.data() + p, 4); p += 4;
  if (!chainSize || uint64_t(chainSize) > n - p) throw exFailedToDecompressNode();
  size_t end = p + size_t(chainSize);
  while (p < end) {
    void const * wordEnd = memchr(decoded.data() + p, 0, end - p);
    if (!wordEnd) throw exFailedToDecompressNode();
    p = size_t(static_cast<char const *>(wordEnd) - decoded.data()) + 1;
    void const * prefixEnd = memchr(decoded.data() + p, 0, end - p);
    if (!prefixEnd) throw exFailedToDecompressNode();
    p = size_t(static_cast<char const *>(prefixEnd) - decoded.data()) + 1;
    if (4 > end - p) throw exFailedToDecompressNode();
    p += 4;
  }
  if (p != end) throw exFailedToDecompressNode();
}
if (p != n) throw exFailedToDecompressNode();
out.swap(decoded);
```

Use `memcpy` for every traversal child/size/offset read that may be unaligned, not just the validator. Preserve successful `readChain` semantics on live validated buffers and its guard before the first read. Do not impose separator ordering/non-empty-word semantics. Cache-loading flags change only after successful decode.

- [ ] **Step 4: Run GREEN shape, descendant, and compatibility probes**

Run T05–T16/T27–T30's decoder/throwing-lookup/bulk assertions; T30's request-error observation and T31's empty-child traversal assertions are implemented and run in Task 4, not counted as gates for this shape-only task. Build one indexed key `word00000` with exactly 1,500,000 article offsets: assert `4+4+1500000*15 = 22500008` decoded bytes, >16 MiB, every returned offset in order. Alternatively choose `word000` and pin exactly 19,500,008 bytes consistently. Run UBSan for unaligned fields and T29 high-ratio positive. Never weaken that positive to make a fixed cap pass.

- [ ] **Step 5: Commit**

```bash
git add btreeidx.cc
git commit -m "fix(btree): validate node and chain shape before traversal"
```

## Task 4: Share cycle-safe leaf transitions and report prefix failures

**Implementer tier:** Frontier
**Lane:** lane-core
**Owns:** btreeidx.hh, btreeidx.cc
**Depends on:** 3

**Files:**
- Modify: `btreeidx.hh:113-157`, `212-250`; `btreeidx.cc:250-442`, `514-853`, `1263-1518`.
- Test, disposable: T17 `next_leaf_internal_node_throws_not_exit`, T18 `self_child_node_cycle_throws`, T19 `two_node_cycle_throws`, T21 `next_leaf_internal_node_from_prefix_walk_throws`, T22 `next_link_to_empty_leaf_throws`, T31 `empty_root_allowed_empty_child_rejected`, T32 `leaf_link_cycles_terminate`, T33 `bulk_scan_more_than_4096_leaves_succeeds`, T44 `prefix_worker_reports_error_and_finishes`; complete T25's new-helper row. Committed slots: Tasks 6–7.

**Interfaces:**
- Consumes `readNode(uint32_t,vector<char>&)` from Tasks 2–3, Task 1's checked lookup/guards, existing request `setErrorString`, `dataMutex`, `finish`, and runnable exit semaphore.
- Produces protected `static void enterNode(TraversalState&,uint32_t)`; `char const *readNextLeaf(uint32_t&,vector<char>&,char const*&,TraversalState&)`; `findChainOffsetExactOrPrefix(wstring const&,bool&,vector<char>&,uint32_t&,char const*&,TraversalState* = 0)` with optional supplied state. No serialized/new shared request state.

- [ ] **Step 1: Write all four transition routes and worker completion RED probes**

Build exactly 130 words `word00000`–`word00129`, offsets 0–129; assert m equals `BtreeMinElements`, first leaf ends at `word00001` and next separator is `word00002`. Rewrite first leaf trailer to internal root and append >=4 readable bytes after root payload. Exercise real prefix scan from `word00000` with prefix `word`, exposed gap lookup `word00001z`, `findArticleLinks`, and an offset request requiring a later leaf. Repeat with an appended `record(W32(0))+W32(0)` target and with missing/1/2/3-byte trailers. Record target-specific `readChain` entry count zero. Save buffer, leafEnd and nextLink before a failed advance and require them unchanged.

```cpp
TraversalState state;
uint32_t next = badTarget;
vector<char> old = leaf;
char const * oldEnd = leafEnd;
QVERIFY_EXCEPTION_THROWN(index.readNextLeaf(next, leaf, leafEnd, state), exCorruptedChainData);
QCOMPARE(next, badTarget); QCOMPARE(leaf, old); QCOMPARE(leafEnd, oldEnd);
```

Cycle rows include self/two internal children and self/two leaf links in lookup, real prefix and both bulk routes. Empty root succeeds; empty descent target fails in all three descents. Prefix failure asserts one `finished`, non-empty error, cleared partial matches, runnable retired; normal no-match/cancel controls remain normal.

- [ ] **Step 2: Run RED with 5-second subprocess limits and pinned route observations**

Run the named probes via the out-of-tree helper. Verify padding prevents short-read masking in T17/T21. Run the 4,100-linked-leaf positive before adding cycle tracking. No test may count an assertion-enabled abort/timeout as the required final contained exception.

- [ ] **Step 3: Implement the traversal transaction and worker error boundary**

`enterNode` is exactly an insertion-and-throw contract:

```cpp
if (!state.visitedOffsets.insert(offset).second) throw exCorruptedChainData();
```

Enter root even on cache hits, enter each child in all three descents, and retain that same state across lookup's own advance and continuing prefix scan. Restart a fresh state for each suffix-chopping root restart; bulk state lasts the full scan. Do not charge the same already-loaded logical node twice. Require a non-empty leaf after a non-root descent.

In protected `readNextLeaf`, invoke the ordered guards before reading arguments; `nextLeaf==0` returns null unchanged. Lock the existing mutex, copy target, enter it, decode to local vector, copy first word, require `first!=0xffffffff && 1<=first && first<=indexNodeSize`. Then prove/read the following four-byte trailer at decoder's current position; missing trailer throws `File::exReadError`. Only then swap leaf and commit leafEnd/nextLeaf/return pointer. Replace all four existing advances with this helper; ordinary internal descent never reads a trailer. Compile the real `BtreeWordSearchRequest` call through `BtreeDictionary` friendship.

Wrap non-empty-chain processing in `if (!chain.empty())` and then perform normal pointer/advance progress; never `continue` on empty without advancing. Replace worker `exit(1)` by the shared helper. In std/catch-all request catches set non-empty error, clear partial matches under `dataMutex`, and return to the single normal `finish()` path. Preserve cancellation and exit-semaphore release.

- [ ] **Step 4: Run GREEN through every route and long-scan/retry controls**

Run T17–T19/T21–T22/T25/T30–T33/T44 with normal child exit checks; T22 trailer rows require `File::exReadError` and unchanged state, not just any exception. Long-scan assertions cover `findAllArticleLinks`, `getAllHeadwords`, `findArticleLinks`, `getHeadwordsFromOffsets` beyond leaf 4,096. Run suffix retries with a fresh state and cancellation. Compile real production request code, not only exposed test methods.

- [ ] **Step 5: Commit**

```bash
git add btreeidx.hh btreeidx.cc
git commit -m "fix(btree): make leaf traversal transactional and cycle safe"
```

## Task 5: Add the pure suite, fixture architecture, and state contract slots

**Implementer tier:** Standard
**Lane:** lane-tests
**Owns:** tests/**
**Depends on:** 4

**Files:**
- Create: `tests/tst_btree_index/{tst_btree_index.pro,tst_btree_index.cc,fixtures.hh,fixtures.cc,owner_adapters.hh}`.
- Modify: `tests/tests.pro:1-10`, `114-143` (closed inventory 18 → 19).
- Read only: `tests/testsupport.pri`, `tests/support/fixtures.hh`, `tests/tst_regex/tst_regex.pro`.

**Interfaces:**
- Consumes all Task 1/4 public/protected APIs with the exact signatures stated there, including optional `TraversalState*` and protected four-argument `readNextLeaf`.
- Produces `TestIndex : BtreeIndex` with `using` declarations for protected decoder/chain/lookup/helper/state; `BtreeIndexTestAccess` is only the named test friend. Produces `TreeFixture` owning `QTemporaryDir`, `File::Class` opened `w+b`, `Mutex`, `TestIndex`, bootstrap `IndexInfo`; `uint32_t append(QByteArray const&)` seeks to EOF, checks uint32 representability, writes non-empty bytes, flushes via `file()`, returns starting offset. `buildWords(size_t)` uses production `addSingleWord/buildIndex`, not handwritten tree assembly. `runChild(QStringList,int milliseconds=5000)` uses QProcess, deadline, kill/wait cleanup and normal-exit/zero-code assertions.
- Implements slots T01 `builds_and_reads_back`, T02 `open_index_terminates`, T04 `insane_index_info_is_inert`, T23 `reopen_valid_then_invalid_then_valid`, T24 `default_constructed_is_unopened`, T25 `every_entry_point_guarded_after_invalid_open`, T26 `shared_file_position_restored_and_no_deadlock`.

- [ ] **Step 1: Write the seven slot bodies against the fixture API first**

Use Task 1's literal state assertions, plus never-opened/unusable matrices covering seven original and two new entries. Capture warning text `Articles searching failed`, verify unchanged sentinel arguments and zero decoder I/O. T26 asserts position immediately under the lock after valid/corrupt probes, calls with already-held recursive mutex, coordinates validation/traversal threads with barriers, joins both inside the deadline, and verifies correct links. T01 includes empty, one-word, alias/prefix/case-folded and multi-leaf production trees. T23 inspects empty/unloaded cache after invalid and usable reopens in both file variants. T02 child does sane open AND expected checked query.

- [ ] **Step 2: Run RED for the missing fixture/suite integration**

Configure/build in `$GD_BUILD_ROOT/t5-red`; missing project/fixture support is the harness RED, not evidence about production validation. Also replay the original unsafe state implementation in a disposable copy retaining declarations; require T04/T23's runtime expectations to fail once the harness builds. Do not mutate the active core files.

- [ ] **Step 3: Implement framing helpers, pure main and real-source linkage**

Use these framing helpers (append the native W32 bytes with memcpy, never a copied shape validator):

```cpp
QByteArray W32(uint32_t x) {
  QByteArray b(4, '\0'); memcpy(b.data(), &x, 4); return b;
}
QByteArray link(QByteArray const &w, QByteArray const &p, uint32_t a) {
  return w + QByteArray(1, '\0') + p + QByteArray(1, '\0') + W32(a);
}
QByteArray record(QByteArray const &payload) {
  uLongf n = compressBound(uLong(payload.size()));
  QByteArray z(qsizetype(n), '\0');
  int rc = compress2(reinterpret_cast<Bytef *>(z.data()), &n,
                    reinterpret_cast<Bytef const *>(payload.constData()),
                    uLong(payload.size()), Z_BEST_COMPRESSION);
  if (rc != Z_OK) throw std::runtime_error("fixture compress2 failed");
  z.resize(qsizetype(n));
  return W32(uint32_t(payload.size())) + W32(uint32_t(z.size())) + z;
}
```

Fixture sizes are checked before helper narrowing; large payloads here fit uint32. Bootstrap is `record(W32(1)+W32(link("a","",7).size())+link("a","",7))+W32(0)` with sane fan-out/root. Open it before appended malformed reads. Define main with `QCoreApplication`, `QTest::qExec`; subprocess modes dispatch before QTest and use real production methods. `initTestCase` requires `TestSupport::requiredEnvironment`, installs 60-second watchdog; cleanup clears it. No QApplication/Chromium initialization/display requirement.

Start the .pro with:

```qmake
TARGET = tst_btree_index
include(../testsupport.pri)
DEFINES += GD_BTREE_INDEX_TEST
QT += core xml
SOURCES += tst_btree_index.cc fixtures.cc \
    $$GD_REPO_ROOT/btreeidx.cc $$GD_REPO_ROOT/chunkedstorage.cc \
    $$GD_REPO_ROOT/file.cc $$GD_REPO_ROOT/dictionary.cc $$GD_REPO_ROOT/mutex.cc \
    $$GD_REPO_ROOT/utf8.cc $$GD_REPO_ROOT/wstring.cc $$GD_REPO_ROOT/wstring_qt.cc \
    $$GD_REPO_ROOT/folding.cc $$GD_REPO_ROOT/wildcard.cc $$GD_REPO_ROOT/fsencoding.cc \
    $$GD_REPO_ROOT/gddebug.cc $$GD_REPO_ROOT/indexedzip.cc \
    $$GD_REPO_ROOT/zipfile.cc $$GD_REPO_ROOT/splitfile.cc
HEADERS += $$GD_REPO_ROOT/dictionary.hh fixtures.hh owner_adapters.hh
LIBS += -lz
unix {
    QMAKE_CXXFLAGS += -ffunction-sections -fdata-sections
    QMAKE_LFLAGS += -Wl,--gc-sections
}
```

Add actually referenced dependencies as owner closures are linked, following application libraries/config and real Q_OBJECT MOC headers. Do not link `tests/support/dictionary_stub.cc`. Define WorkerCase/WorkerResult AND PairObservation/PairReport in `owner_adapters.hh` now with the exact fields/signatures from Global Constraints, including `QString variant` and the Prefix/Fts routes, so later producer lanes consume real types rather than redefining them. Value-initialize observation/result fields; actual asynchronous routes overwrite completion/retirement observations, while instant routes use the direct completed-state contract. Keep MOC/UI/RCC/objects in inherited external target paths. Add the suite name in sorted position in the closed required list and change count, diagnostics and comments to 19; do not change to open-ended discovery.

- [ ] **Step 4: Run GREEN plus environment/child harness self-checks**

`step s2 builds_and_reads_back foreign_sounddir_header_is_not_sane insane_index_info_is_inert open_index_terminates` is the spec's eventual step-2 gate; T03 is added in Task 16 after actual guard wiring. For this task run `step t5 builds_and_reads_back open_index_terminates insane_index_info_is_inert reopen_valid_then_invalid_then_valid default_constructed_is_unopened every_entry_point_guarded_after_invalid_open shared_file_position_restored_and_no_deadlock`. Assert environment force-unavailable causes failure (not skip) in a separate process and watchdog probe is bounded. Configure aggregate externally and verify exactly 19 projects including this target, even before the later slots exist.

- [ ] **Step 5: Commit**

```bash
git add tests/tests.pro tests/tst_btree_index
git commit -m "test(btree): add pure suite and index state fixtures"
```

## Task 6: Pin decoder bounds, exact shape, and writer compatibility

**Implementer tier:** Advanced
**Lane:** lane-tests
**Owns:** tests/**
**Depends on:** 5

**Files:**
- Modify: `tests/tst_btree_index/{tst_btree_index.cc,fixtures.hh,fixtures.cc}` (Task 5's slot/fixture blocks).

**Interfaces:**
- Consumes `TreeFixture::append(QByteArray const&):uint32_t`, exposed `readNode(uint32_t,vector<char>&)`, checked lookup/predicate and `W32/link/record` from Task 5 and the production contracts from Tasks 1–4.
- Implements T05 `zeroed_node_record_throws`, T06 `short_node_record_throws`, T07 `truncated_node_offsets_array_throws`, T08 `unterminated_separator_throws`, T09 `absurd_leaf_entry_count_throws`, T10 `leaf_framing_not_exact_throws`, T11 `zero_size_chain_throws`, T12 `truncated_inner_chain_record_throws`, T13 `out_of_range_node_offset_throws`, T14 `oversized_btree_size_rejected`, T15 `max_node_ceiling_rejects_before_allocation`, T16 `builder_large_node_reads_back`, T27 `internal_node_consumes_exact_separator_region`, T28 `compressed_record_bounds_use_wide_arithmetic`, T29 `incremental_inflate_checks_stream_and_length`, T30 `corrupt_descendant_rejected_after_sane_root`, T31 `empty_root_allowed_empty_child_rejected`.

- [ ] **Step 1: Write the slot assertions and named data rows first**

Use all Task 2/3 byte matrices verbatim, including every offset-shortness variant and exact-size positives. T14 tests m=0/max+1/UINT32_MAX false and structurally valid m=1/max true. T27 builds the complete `4+(m+1)*4` array and exactly m non-empty separators, then adds/removes NUL/bytes. T28 records RSS before/after sparse forged-C cases, position under lock, and actual apparent file size. T29 includes bad checksum/truncation/tail/no-progress, both U mismatches, small byte-identical success and the exact high-ratio leaf.

```cpp
QCOMPARE(payload.size(), qsizetype(32 * 1024 * 1024));
QVERIFY(uint64_t(payload.size()) > uint64_t(compressedSize) * 516u + 64u);
QVERIFY(uint64_t(payload.size()) <= uint64_t(compressedSize) * 1032u + 64u);
f.index.readNode(appendedOffset, decoded);
QCOMPARE(QByteArray(decoded.data(), qsizetype(decoded.size())), payload);
```

T30 first asserts root predicate true, opens it, then reaches the corrupt descendant through checked lookup, bulk and a real prefix request. T31 tests each of three descent paths with empty child and builder-empty-root success. T16 MUST use the production builder with the exact 1,500,000-link size/ordered-results assertions, not only a successful decode.

- [ ] **Step 2: Run RED before implementing new fixture generators**

Compile the slot bodies against declarations of sparse-file/internal-node/large-chain generators to establish missing-fixture RED. Once generators exist, run the inner-chain-walk-disabled disposable control and require T12 runtime failure; do not call an already-green implementation a new production RED. Preserve original pre-fix subprocess evidence from Tasks 2–3 as well.

- [ ] **Step 3: Implement fixture generators without validation duplication**

Generators serialize offsets/separators/chains, compress with zlib, and mutate specified bytes. Use production builders for ordinary, empty, 130-word and large-chain trees; hand-framing is permitted only for the explicitly specified structural/sparse/cycle fixtures. The malformed decoder always receives an appended offset after sane bootstrap opening. Retain unchanged-output assertions after errors and root-cache loaded-state observations. Never introduce a `MaxNodeBytes` expectation because of the legacy T15 slot name.

- [ ] **Step 4: Run GREEN in ordinary and isolated sanitizer output directories**

Run the full slot list above, including T13/T14/T27–T31 omitted from the spec's abbreviated `s3` command. Also execute the literal s3 selection through `step s3 zeroed_node_record_throws short_node_record_throws truncated_node_offsets_array_throws unterminated_separator_throws absurd_leaf_entry_count_throws leaf_framing_not_exact_throws builder_large_node_reads_back max_node_ceiling_rejects_before_allocation`. No timed-out/killed child passes; UBSan must be clean for deliberately unaligned fields.

- [ ] **Step 5: Commit**

```bash
git add tests/tst_btree_index
git commit -m "test(btree): cover malformed records and large valid nodes"
```

## Task 7: Pin all transition routes, cycles, and prefix request completion

**Implementer tier:** Advanced
**Lane:** lane-tests
**Owns:** tests/**
**Depends on:** 6

**Files:**
- Modify: `tests/tst_btree_index/{tst_btree_index.cc,fixtures.hh,fixtures.cc,owner_adapters.hh}`.

**Interfaces:**
- Consumes protected `readNextLeaf(uint32_t&,vector<char>&,char const*&,TraversalState&)`, lookup with `TraversalState*`, bulk methods and real `BtreeDictionary::prefixMatch/stemmedMatch` from Task 4; Task 5 fixture and child helpers.
- Implements T17 `next_leaf_internal_node_throws_not_exit`, T18 `self_child_node_cycle_throws`, T19 `two_node_cycle_throws`, T21 `next_leaf_internal_node_from_prefix_walk_throws`, T22 `next_link_to_empty_leaf_throws`, T32 `leaf_link_cycles_terminate`, T33 `bulk_scan_more_than_4096_leaves_succeeds`, T44 `prefix_worker_reports_error_and_finishes`.

- [ ] **Step 1: Write route-specific assertions before support code**

Pin the 130-word tree, first-leaf last word/next separator, internal-root padding, matching-prefix and exact gap routes from Task 4. T22's `_data` has four routes × empty/internal targets, and absent/1/2/3-byte trailer rows. Empty targets must fail before target `readChain` entry, not merely avoid an ASan error. Trailer failure must be `File::exReadError` with unchanged leaf/leafEnd/next-link. T18/T19/T32 child modes run checked and bulk paths with 5-second wall budgets and require normal exit. T33 has 4,100 individually framed unique leaves, sane m=1 root, all four bulk result sets and a suffix-retry control. T44 observes a real request, including inert and sane-root/corrupt-child variants, exactly one completion, non-empty error, no partial success, and runnable retirement; include no-match/cancellation controls.

```cpp
QSignalSpy done(request.get(), &Dictionary::Request::finished);
QTRY_VERIFY_WITH_TIMEOUT(request->isFinished(), 4000);
QCOMPARE(done.count(), 1);
QVERIFY(!request->getErrorString().isEmpty());
QCOMPARE(request->matchesCount(), size_t(0));
```

Arrange the spy before scheduling via the test fixture's stopped-start hook so a fast completion is not lost; use actual declared request accessor types when binding assertions.

- [ ] **Step 2: Run RED for missing generators and falsify the gap route**

The new multi-route generator/observer declarations initially fail to link. Implement them, establish a clean green baseline, then in a disposable copy bypass only the prefix-walk helper while preserving other routes; T21 must fail a runtime assertion. This ensures T17's matching route cannot accidentally stand in for T21.

- [ ] **Step 3: Implement real dictionary fixture, child dispatch and target counters**

Implement only unrelated abstract metadata methods of the concrete `BtreeDictionary` fixture; do not stub `findMatches`, decoder, helper or error handling. Use the narrow test friend/counter in the real chain entry to count target reads without changing normal behavior. Serialize traversal observations under the actual mutex. Long-scan generator links each leaf with the next node offset and checks the leaf count, not the chain count. Preserve all child timeout/crash diagnostics.

- [ ] **Step 4: Run GREEN including spec's s4 gate and every route**

`step s4 next_leaf_internal_node_throws_not_exit next_leaf_internal_node_from_prefix_walk_throws next_link_to_empty_leaf_throws self_child_node_cycle_throws leaf_link_cycles_terminate`; additionally run T19/T33/T44 and Task 6's empty-descendant slot. Verify the protected call compiles through the real production prefix class. Check all T22 data rows execute, not merely the slot name.

- [ ] **Step 5: Commit**

```bash
git add tests/tst_btree_index
git commit -m "test(btree): pin leaf transitions cycles and request errors"
```

## Task 8: Harden chunk tables, incremental streams, and block starts

**Implementer tier:** Frontier
**Lane:** lane-storage
**Owns:** chunkedstorage.hh, chunkedstorage.cc
**Depends on:** 4, 7

**Files:**
- Modify: `chunkedstorage.hh:73-92` (reader contract only), `chunkedstorage.cc:111-166` (constructor/getBlock).
- Disposable probes: T20 `zero_size_chunk_throws`, T38 `chunk_table_and_block_ranges_are_bounded`, T39 `writer_oversized_chunk_block_reads_back`. Committed slots: Task 11. Consumes Task 5's production-linked fixture/subprocess harness, so pre-fix assertions stay outside the aggregate.

**Interfaces:**
- Preserve `Reader(File::Class&,uint32_t)` and `char *getBlock(uint32_t,vector<char>&)`; use the caller's existing file lock. Table/address errors are `exAddressOutOfRange`; corrupt record/stream/length and record I/O are `exFailedToDecompressChunk`. No private mutex and no btree fan-out dependency.

- [ ] **Step 1: Write RED table/frame/address and writer-positive probes**

Matrix: table offset/count short/past EOF, count product, selected chunk index, record offset/header/C past EOF, stream/U mismatch, block start > actual output. Table pointing to `{0,0}` constructs but getBlock throws. Valid empty table, real empty zlib chunk at start zero, and offset exactly at end remain legal. Production writer starts one block, appends 65,537 known nonzero bytes, finishes/reopens, compares all bytes; also test a larger compressible block.

```cpp
uint32_t address = writer.startNewBlock();
writer.addToBlock(pattern.constData(), size_t(pattern.size()));
uint32_t table = writer.finish();
ChunkedStorage::Reader reader(file, table);
vector<char> chunk;
char * p = reader.getBlock(address, chunk);
QCOMPARE(QByteArray(p, pattern.size()), pattern);
```

- [ ] **Step 2: Run RED with assertions enabled**

Use the external probe suite for the three slots; require malformed inputs to reach actual `Reader` methods, not a copied bounds helper. Record original `{0,0}` child abort separately. Positive oversized readback must already work and must remain so.

- [ ] **Step 3: Implement range-before-resize and exact incremental decode**

Bound table offset, four-byte count, wide count*4, vector max-size before resize. In getBlock check table index then U/C header and C region with subtraction; require C>0, permit U=0, apply `uint64_t(C)*1032+64`, container/API checks and the actual-output loop/RAII from Task 2 without importing node shape/fan-out. Decode locally; clear/unpublish partial output on chunk failure. Require blockOffset<=actualChunkSize. For empty chunk offset zero return `chunk.data()` without adding zero; other legal one-past starts may be returned but never dereferenced by the reader. Do not add `U<=ChunkMaxSize`.

- [ ] **Step 4: Run GREEN and regress core suite**

Run T20/T38/T39 probes and the committed core suite from Task 7 in fresh build dirs. Verify invalid table/address and invalid stream produce their distinct exception types, and failure output does not leak partially inflated bytes. Run zero/one-past and 65,537-byte positives with assertions and sanitizers.

- [ ] **Step 5: Commit**

```bash
git add chunkedstorage.hh chunkedstorage.cc
git commit -m "fix(storage): bound chunk tables streams and block starts"
```

## Task 9: Bound independent ZIP framing and raw-deflate decoding

**Implementer tier:** Frontier
**Lane:** lane-storage
**Owns:** indexedzip.hh, indexedzip.cc
**Depends on:** 8, 4

**Files:**
- Modify: `indexedzip.hh:22-40` (document missing versus malformed contract), `indexedzip.cc:30-111`.
- Disposable probes: T40 `indexed_zip_stored_entries_are_size_checked`, T41 `indexed_zip_deflate_is_incremental_and_exact`. Committed slots: Task 11; owning request behavior is completed in Tasks 17–18 and committed in Task 23.

**Interfaces:**
- Consumes Task 4's inherited checked lookup/inert opening and Task 8's tested empty-output convention, not chunk framing/limits.
- Preserve both `bool IndexedZip::loadFile(gd::wstring const&,std::vector<char>&)` and `bool loadFile(uint32_t,std::vector<char>&)`; malformed/unsupported records return false and clear output. `hasFile`/name lookup must expose corrupt btree errors via checked lookup; a genuine absent name is still false. Known referenced false loads become owner request errors, never normal misses.

- [ ] **Step 1: Write actual ZIP RED probes**

Generate little-endian ZIP local headers and separate source streams: method 0 empty/65,537-byte positives, unequal C/U, local-header/name/extra truncation, source offset/payload past EOF; method 8 raw deflate empty/large positives, malformed/truncated stream, U mismatch, tail, expansion violation and unsupported method. Prefill output with stale bytes and require false clears them.

```cpp
vector<char> data(3, 'x');
QVERIFY(!zip.loadFile(badLocalHeaderOffset, data));
QVERIFY(data.empty());
QVERIFY(zip.loadFile(validEmptyOffset, data));
QVERIFY(data.empty());
```

- [ ] **Step 2: Run RED against real IndexedZip**

Build probe with `indexedzip.cc`, `zipfile.cc`, `splitfile.cc`, real btree and required dependencies. Exercise both overloads, not only a local length function. Record actual old false-success/OOB or wrong-empty behavior in child processes.

- [ ] **Step 3: Implement local-header/file/codec checks**

Clear output before work. Reject negative source sizes/positions; bound fixed header, filename and extra regions before deriving payload position; require payloadPos<=zipSize and C<=zipSize-payloadPos using wide arithmetic/explicit little-endian conversion. Stored requires C==U, checked receiving size, and skips read/copy for zero bytes. Deflate uses `inflateInit2(-MAX_WBITS)`, the same wide format-derived bound, incremental actual growth, exact U, consumed C, Z_STREAM_END and RAII cleanup; no int narrowing of compressedSize. Unsupported/malformed returns false with empty output; unexpected checked btree errors propagate to the owner's catch. ZIP resources do not use `ChunkMaxSize`.

- [ ] **Step 4: Run GREEN and positive compatibility controls**

Run T40/T41 probes and the committed core suite; replay Task 8's saved T20/T38/T39 fixture source in this disposable probe, because those chunk slots are not committed until Task 11. Compare large stored/deflated content byte-for-byte and require valid empty entries true. Tasks 17–18 must demonstrate that an owning resource request reports a referenced malformed entry, and Task 23 commits that assertion; this decoder task must not claim owner coverage by itself.

- [ ] **Step 5: Commit**

```bash
git add indexedzip.hh indexedzip.cc
git commit -m "fix(zip): validate local entries and inflate exact raw streams"
```

## Task 10: Bound MDX/MDD source records and adjacent metadata

**Implementer tier:** Frontier
**Lane:** lane-producers
**Owns:** mdx.cc, mdictparser.cc
**Depends on:** 8, 4, 5

**Files:**
- Modify: `mdx.cc:107-184`, `406-483`, `516-528`, `613-720`, `791-921`, `928-1005`.
- Modify: `mdictparser.cc:240-311` (existing codec dispatch and size agreement).
- Disposable probes: T42 `mdd_record_slice_is_bounded`, T43 `mdx_article_record_slice_is_bounded`, T49 `mdx_stylesheet_records_are_bounded`, T50 `mdx_description_is_bounded`. Committed slots: Task 11.

**Interfaces:**
- Consumes Task 8 `Reader::getBlock(uint32_t,vector<char>&)` including legal empty/end pointers, Task 4 checked lookup, `MdictParser::RecordInfo`'s existing signed fields and `parseCompressedBlock(qint64,char const*,qint64,QByteArray&)` (retain actual declaration's const qualification).
- Preserve real `IndexedMdd::loadFile`, `MdxDictionary::loadArticle`, deferred-init and description interfaces. Add only test forwards `Mdx::btreeTestWorker(WorkerCase const&):WorkerResult` under the global adapter contract, plus narrow real-loader observation routes; no new serialized fields/header edits.

- [ ] **Step 1: Write record-slice and metadata RED probes first**

For MDD AND MDX article paths enumerate short RecordInfo suffix, every negative signed field, mapped compressed position/length outside source, representation extremes, actual D mismatch, recordOffset>D, size>D-offset; zero-sized record exactly at D and exact nonzero slice succeed. Recompress outer chunk so errors reach actual record readers. Stylesheet cases are zero begin/end, count beyond remaining file, length past EOF and bytes with no NUL. Description tests set descriptionSize shorter than a larger chunk and exactly zero, asserting exact resulting text.

```cpp
QVERIFY(Mdx::btreeTestWorker(shortRecordCase).error.size() > 0);
QCOMPARE(Mdx::btreeTestWorker(zeroDescriptionCase).text, QString());
QCOMPARE(Mdx::btreeTestWorker(shortDescriptionCase).text, QStringLiteral("abc"));
```

- [ ] **Step 2: Run RED against actual loaders and init/request boundaries**

Use the Task 5 fixture harness with real `mdx.cc`/`mdictparser.cc`/codec dependencies; test-only forwards call real methods. For short/private reader cases verify production reader entry. For request cases observe error, one finish and runnable exit, not only a thrown helper exception.

- [ ] **Step 3: Implement signed/source/slice and metadata proofs**

Prove chunk non-empty and full `sizeof(RecordInfo)` suffix before memcpy. Reject negative fields before conversion, validate source mapping region by subtraction and API/container sizes before map/decode. Retain plain/zlib/supported-LZO dispatch and checksum behavior; zlib must not preallocate advertised output, plain/LZO use their own bounds. Require actual output equals advertised decompressed size; then require offset<=D and size<=D-offset. Zero result succeeds without `front()` or pointer addition/copy. Apply identical proof to article loader, not just MDD.

Stylesheet parser proves minimum bytes/count, key and each length against index file before resize; zero skips read/front; use explicit byte count (and a checked optional terminator), never NUL-dependent `QString::fromUtf8(buf.data())`. Bound description by `descriptionSize` within actual chunk suffix, then construct `std::string(ptr,size)` only when nonzero. Propagate failures through deferred-init error and owning request. Extend outer article/MDD catches to include initial checked lookup and all size operations, set error/clear partial success under mutex/catch-all/normal completion. Redirect-cycle cleanup is Task 22.

- [ ] **Step 4: Run GREEN all four slots with codec positives**

Run T42/T43/T49/T50 probes with short-boundary and zero controls. Compile/run existing plain/zlib and installed LZO configuration; no deflate cap on LZO/plain. Assert article/MDD requests finish/error rather than success HTML from corrupted input. Keep Task 8's valid empty chunk/end pointer behavior unchanged.

- [ ] **Step 5: Commit**

```bash
git add mdx.cc mdictparser.cc
git commit -m "fix(mdict): bound source slices stylesheets and descriptions"
```

## Task 11: Commit storage, ZIP, MDX/MDD, and metadata acceptance slots

**Implementer tier:** Advanced
**Lane:** lane-tests
**Owns:** tests/**
**Depends on:** 7, 8, 9, 10

**Files:**
- Modify: `tests/tst_btree_index/{tst_btree_index.pro,tst_btree_index.cc,fixtures.hh,fixtures.cc,owner_adapters.hh}`.

**Interfaces:**
- Consumes unchanged `ChunkedStorage::Reader/Writer`, both `IndexedZip::loadFile` overloads, and `Mdx::btreeTestWorker(WorkerCase const&):WorkerResult` from Tasks 8–10; source codec forwarding must call actual `parseCompressedBlock`.
- Implements T20 `zero_size_chunk_throws`, T38 `chunk_table_and_block_ranges_are_bounded`, T39 `writer_oversized_chunk_block_reads_back`, T40 `indexed_zip_stored_entries_are_size_checked`, T41 `indexed_zip_deflate_is_incremental_and_exact`, T42 `mdd_record_slice_is_bounded`, T43 `mdx_article_record_slice_is_bounded`, T49 `mdx_stylesheet_records_are_bounded`, T50 `mdx_description_is_bounded`.

- [ ] **Step 1: Write the nine slots/data matrices before fixture extensions**

Promote every Task 8–10 probe recipe, including empty table versus zero stream, one-past offset, oversized writer block, little-endian ZIP framing, raw/zlib separation, both actual MDX/MDD source paths, zero-at-D record, stylesheet count/length/no-NUL cases and descriptionSize. T41's request assertion is completed when owning worker forwards land; do not replace it with a boolean decoder-only assertion.

```cpp
QVERIFY_EXCEPTION_THROWN(reader.getBlock(zeroStreamAddress, chunk), ChunkedStorage::exFailedToDecompressChunk);
char * end = reader.getBlock(legalOnePastAddress, chunk);
QCOMPARE(reinterpret_cast<uintptr_t>(end), reinterpret_cast<uintptr_t>(chunk.data()) + chunk.size());
```

For a genuinely empty vector avoid even test-side null-pointer addition; test returned pointer equality with `chunk.data()` directly.

- [ ] **Step 2: Run RED for missing fixture/owner linkage, then a runtime control**

First build reveals missing actual-owner fixture support. Once linked, prove the forged-slice variant fails against an isolated old slice expression, and the restored-65,536-chunk-cap variant fails T39 with a clean baseline. Build errors are harness RED only, not killed production mutants.

- [ ] **Step 3: Implement fixture/link closures and real-owner adapters' declarations**

Add real `mdx.cc`, `mdictparser.cc`, decompression, scoped mapping, ZIP and actual required dependencies to the .pro; MOC production Q_OBJECT headers and mirror actual application feature libraries. Use the existing global WorkerCase/WorkerResult types and extend fixture construction; production forwards remain owned by `lane-producers`. Use boundary recipes with final suffixes 0..k-1, exact k, oversized following lengths and zero controls. Keep no external dictionary/network dependency. Build feature variants for supported codecs, never silently skip an uncompiled branch and claim coverage.

- [ ] **Step 4: Run GREEN storage/metadata and prior core slots**

Run all nine named slots plus the full committed suite. Run spec s7's already available MDX selections explicitly: `mdx_stylesheet_records_are_bounded mdx_description_is_bounded mdx_article_record_slice_is_bounded mdd_record_slice_is_bounded`. T41 owner-reporting evidence is finalized in Tasks 17–18/23; decoder assertions must be green here. Keep sanitizer output directories separate.

- [ ] **Step 5: Commit**

```bash
git add tests/tst_btree_index
git commit -m "test(storage): pin chunks zip slices and mdict metadata"
```

## Task 12: Route simple primary producer failures into existing rebuild branches

**Implementer tier:** Advanced
**Lane:** lane-producers
**Owns:** sounddir.cc, sdict.cc, aard.cc, bgl.cc, dictdfiles.cc, lsa.cc, zipsounds.cc, epwing.cc
**Depends on:** 10

**Files:**
- Modify: `sounddir.cc:51-74,418-451`, `sdict.cc:95-127,742-780`, `aard.cc:105-140,839-878`, `bgl.cc:61-105,1170-1210`.
- Modify: `dictdfiles.cc:78-105,575-610`, `lsa.cc:62-90,531-580`, `zipsounds.cc:59-88,151-173,418-450`, `epwing.cc:51-85,997-1040`.
- Disposable probes: T03 `foreign_sounddir_header_is_not_sane`; the eight corresponding T34 `every_primary_producer_routes_bad_info_to_rebuild` rows; ZipSounds T37 `zip_secondary_pair_uses_owning_index_file` control. Committed slots: Task 16.

**Interfaces:**
- Consumes public `static bool BtreeIndex::indexInfoIsSane(IndexInfo const&,File::Class&,Mutex&)` and nonthrowing inert `openIndex(IndexInfo const&,File::Class&,Mutex&)` from Task 1.
- Produces unchanged existing `indexIsOldOrBad(std::string const&)` guards, each returning true on header/open/root failures; no change to its rebuild branch/signature/version checks.
- Under `GD_BTREE_INDEX_TEST`, expose `bool <Owner>::btreeTestOldOrBad(std::string const&)` for `SoundDir`, `Sdict`, `Aard`, `Bgl`, `DictdFiles`, `Lsa`, `ZipSounds`, `Epwing`; each simply returns that translation unit's actual guard. Also expose `BtreeTest::PairReport <Owner>::btreeTestPairs(std::string const&,std::vector<std::string> const&)` through the same bounded header/pair reader for Task 26's census. Pair types are defined in Task 5; a report enumerates actual persisted fields and calls the shared predicate on their owning file, not a test-side parser.

- [ ] **Step 1: Write RED guard probes, including the literal foreign bytes**

Use unchanged identity fields and production-valid root controls for each owner. Invalid fan-out, root header past EOF, missing file and short header must return old-or-bad. The SoundDir test writes the following words, not a synthetic substitute:

```cpp
uint32_t const foreign[] = {0x58524453, 11, 142000, 28, 2057158, 378, 2053531};
for (uint32_t x : foreign) fixtureBytes += BtreeTest::W32(x);
QVERIFY(SoundDir::btreeTestOldOrBad(foreignPath));
QVERIFY(!BtreeIndex::indexInfoIsSane(IndexInfo(2057158, 378), file, mutex));
QVERIFY(!SoundDir::btreeTestOldOrBad(localValidPath));
```

The local valid index is longer than its 24-byte header: total file length is not a header-size discriminator. ZipSounds has ONE persisted pair consumed twice; assert the actual guard accepts it and both inherited open consumers use that same file/pair.

- [ ] **Step 2: Run RED with actual guard forwards**

Add forwards only in the disposable copy first, build the real owner translation units and run each named data row. Old identity-only checks must accept at least one corrupt-pair fixture and fail the new expectation. An uncalled owner or string scan for `indexInfoIsSane` does not count.

- [ ] **Step 3: Extend guards with exception-contained OR validation**

Apply this pattern to each of the eight actual guard bodies while preserving each original identity/version/parser/folding test:

```cpp
try {
  File::Class idx(indexFile, "rb");
  IdxHeader header;
  if (idx.readRecords(&header, sizeof(header), 1) != 1) return true;
  if (header.signature != Signature || header.formatVersion != Version) return true;
  Mutex mutex;
  return !BtreeIndexing::BtreeIndex::indexInfoIsSane(
      IndexInfo(header.indexBtreeMaxElements, header.indexRootOffset), idx, mutex);
} catch (...) { return true; }
```

Use each file's actual constants and exact field spellings; SoundDir's fields are `indexBtreeMaxElements/indexRootOffset` in the current header. Do not create the exemplar identifiers `Signature`/`Version` in production. If factoring a file-local header/pair reader for census forwards, both guard and forward use it; forwards must not reimplement validation. File-local guard-owned files get one local mutex; never switch to a source/ZIP file. Verify existing makeDictionaries branches still consume true without changing builder bytes. Keep EPWING guards inside supported feature configuration; compile it enabled. This task does not yet claim worker-record closure.

- [ ] **Step 4: Run GREEN all eight guards and existing controls**

Run the disposable T03/T34 rows, valid identity mismatch controls and missing/short-file cases. Inspect observed guard-return → existing rebuild routing for every row, including the second ZipSounds open's inert protection. Compile real owners with assertion flags and EPWING support; header/format constants remain byte-identical in the diff.

- [ ] **Step 5: Commit**

```bash
git add sounddir.cc sdict.cc aard.cc bgl.cc dictdfiles.cc lsa.cc zipsounds.cc epwing.cc
git commit -m "fix(indexes): rebuild invalid primary producer trees"
```

## Task 13: Validate every present optional ZIP pair in its owning index

**Implementer tier:** Capable
**Lane:** lane-producers
**Owns:** stardict.cc, dsl.cc, gls.cc, xdxf.cc
**Depends on:** 12, 9, 11

**Files:**
- Modify: `stardict.cc:136-177,290-308,1960-1985`; `dsl.cc:94-158,470-485,2160-2190`.
- Modify: `gls.cc:394-440,560-575,1500-1530`; `xdxf.cc:100-148,285-314,1190-1215`.
- Disposable probes: four T34 primary rows, optional-ZIP T35 `every_secondary_tree_routes_bad_info_to_rebuild` rows, T37 `zip_secondary_pair_uses_owning_index_file`. Committed slots: Task 16.

**Interfaces:**
- Consumes `indexInfoIsSane(IndexInfo const&,File::Class&,Mutex&)`, inherited inert `IndexedZip::openIndex`, and Task 9's independent ZIP-source loading behavior.
- Preserve `Stardict`/`Xdxf` guard `(std::string const&)` and `Dsl`/`Gls` guard `(std::string const&,bool hasZipFile)`; expose exact matching `<Owner>::btreeTestOldOrBad` signatures under the test define. Produce the same `btreeTestPairs(path,sourceFiles):PairReport` census forward as Task 12 for all four owners, reading actual header fields.

- [ ] **Step 1: Write primary/secondary and owning-file RED rows**

For every owner keep the main pair valid and vary only ZIP pair: valid present, `{0,0}` absent, half-present `{0,root}`/`{m,0}` fixtures chosen so the present pair cannot describe that file, invalid fan-out and bad root. Root zero can be valid in the generic API; do not assert every numeric half-pair is invalid independent of actual file content. Preserve source availability/zipSupportVersion controls. Put valid bytes at an identical offset in the source ZIP and corrupt bytes in the owner index; the guard must reject the owner, then invert the bytes to prove the opposite file cannot contaminate validation.

```cpp
bool present = header.zipIndexBtreeMaxElements != 0 || header.zipIndexRootOffset != 0;
QVERIFY(ownerGuard(mainValid_zipCorrupt_ownerFile, true));
QVERIFY(!ownerGuard(mainValid_zipAbsent_ownerFile, false));
```

Here `ownerGuard` is the fixture's typed dispatch to the actual owner's declared forward, not a copied implementation; one-argument owners retain one argument.

- [ ] **Step 2: Run RED in the actual owner closures**

Run all four primary/secondary probes. Require optional corrupt pairs previously ignored by the guards to fail runtime assertions. Confirm T37 observes separate index and ZIP files and executes actual ZIP load as well as guard validation.

- [ ] **Step 3: Extend guards without changing absence/version conventions**

After a complete header read and all original checks, validate main pair, then:

```cpp
if ((header.zipIndexBtreeMaxElements || header.zipIndexRootOffset) &&
    !BtreeIndexing::BtreeIndex::indexInfoIsSane(
       IndexInfo(header.zipIndexBtreeMaxElements, header.zipIndexRootOffset), idx, mutex))
  return true;
```

Both probes use the same owning index file/mutex and OR into old-or-bad. Keep existing Stardict source-ZIP availability, DSL `hasZipFile`/`zipSupportVersion`, GLS ZIP-version condition and XDXF nonzero persisted pair checks. Catch open/read/parse failures to true. Do not wait for an inert secondary open to trigger rebuild, and do not invent a generic IndexedZip header. Compile/forward the actual guards and real inherited opens.

- [ ] **Step 4: Run GREEN primary/secondary/ZIP behavior**

Run every T34/T35/T37 row for these owners, absent and valid empty-tree controls, and Task 11's ZIP decoder slots in a fresh output directory. Confirm source-ZIP reads use the ZIP source while tree predicates use the owning index. No version expressions change.

- [ ] **Step 5: Commit**

```bash
git add stardict.cc dsl.cc gls.cc xdxf.cc
git commit -m "fix(indexes): validate optional zip trees in owner guards"
```

## Task 14: Wire required ZIM/SLOB resource trees and separate FTS files

**Implementer tier:** Advanced
**Lane:** lane-producers
**Owns:** zim.cc, slob.cc, ftshelpers.cc
**Depends on:** 13

**Files:**
- Modify: `zim.cc:135-147,516-531,766-774,1023-1037,1650-1677`; `slob.cc:73-85,141-159,720-727,1133-1148,1730-1757`.
- Modify: `ftshelpers.cc:26-45` (guard only; FTS worker is Task 22).
- Disposable probes: ZIM/SLOB/FTS T34 rows, required-resource/separate-FTS T35 rows. Committed slots: Task 16.

**Interfaces:**
- Consumes `indexInfoIsSane(IndexInfo const&,File::Class&,Mutex&)`; `BtreeDictionary::getFtsMutex():Mutex&`, `getFtsIndexVersion():uint32_t`.
- Preserve `Zim`/`Slob` guard `(std::string const&)` and public `bool FtsHelpers::ftsIndexIsOldOrBad(std::string const&,BtreeIndexing::BtreeDictionary*)`.
- Test forwards: `Zim::btreeTestOldOrBad(path)` and `Slob::btreeTestOldOrBad(path)`, same census forward as Task 12; `FtsHelpers::btreeTestPairs(std::string const&,BtreeIndexing::BtreeDictionary*):PairReport` uses actual FtsIdxHeader and live dictionary mutex/version.

- [ ] **Step 1: Write RED required-pair and FTS-identity probes**

Keep main pair sane; corrupt only `resourceIndexBtreeMaxElements/resourceIndexRootOffset`. A required empty resource tree has a real sane empty root and succeeds; `{0,0}` does not mean absence. Create separate main and `_FTS` files with differing root bytes at the same offsets. Valid FTS version is exactly `CurrentFtsFormatVersion + dict->getFtsIndexVersion()`; mismatch, short/open failures or bad FTS pair returns true while a valid main index is untouched.

```cpp
QVERIFY(FtsHelpers::ftsIndexIsOldOrBad(corruptFtsPath, &dict));
QVERIFY(!FtsHelpers::ftsIndexIsOldOrBad(validFtsPath, &dict));
```

- [ ] **Step 2: Run RED against the supported feature closures**

Enable `CONFIG+=zim_support`, retain EPWING/codec support from the package, and compile actual ZIM/SLOB/FTS guards. The ignored resource/FTS pair must cause a runtime failure of new expectations, not a skipped feature row.

- [ ] **Step 3: Implement both required resources and shared-lock FTS validation**

ZIM/SLOB guards validate both pairs unconditionally after original complete-header checks, OR failures into existing rebuild branches, and contain file/parse exceptions. FTS holds `dict->getFtsMutex()` over ALL operations on its live file window and validates only the `_FTS` file with that same recursive mutex. Keep the exact version sum and existing per-dictionary `makeFTSIndex` callers; do not validate its pair against the main index. If a guard-owned non-live object is used, a local mutex is allowed, but never introduce unsynchronized reads outside the chosen lock. Required empty resource roots remain sane.

- [ ] **Step 4: Run GREEN required and separate-file controls**

Run corresponding T34/T35 rows and all previous producer probes. Verify existing `makeFTSIndex` guards consume the shared result and no failed resource open leaves an unrebuildable inert dictionary. Compile ZIM-enabled and default-disabled configurations with clean, separate outputs.

- [ ] **Step 5: Commit**

```bash
git add zim.cc slob.cc ftshelpers.cc
git commit -m "fix(indexes): validate required resources and separate fts trees"
```

## Task 15: Bound every MDD table record at startup and deferred initialization

**Implementer tier:** Frontier
**Lane:** lane-producers
**Owns:** mdx.cc
**Depends on:** 14

**Files:**
- Modify: `mdx.cc:438-481`, `1377-1389`, `1415-1440`; read writer at `1580-1612` without format edits.
- Disposable probes: MDX T34 row; every-volume T35 rows; T36 `mdd_table_parsing_is_bounded`; T37 owning-main-file control. Committed slots: Task 16.

**Interfaces:**
- Consumes `indexInfoIsSane(IndexInfo const&,File::Class&,Mutex&)`, Task 10's MDX error/request boundary and unchanged MDD/source loader.
- Preserve `bool indexIsOldOrBad(std::vector<std::string> const& dictFiles,std::string const& indexFile)`; expose `Mdx::btreeTestOldOrBad(std::vector<std::string> const&,std::string const&)` in that exact order and `Mdx::btreeTestPairs(path,sourceFiles):PairReport`.
- Produce one file-local bounded MDD name/pair table reader used by BOTH the actual guard and deferred-init loop. Its output is `std::vector<std::pair<std::string,IndexInfo> >`; errors propagate to caller (guard returns old-or-bad; deferred init records initError). No public production/header API addition.

- [ ] **Step 1: Write two-volume parser and deferred-init RED probes**

Serialize two variable-length names/pairs with the actual writer layout. Cases: `dictFiles.empty()`, zero volumes with one main source, count/source disagreement, invalid table offset/count, short length/name/pair, name length consuming the following eight pair bytes, and a good second record after the first predicate restored position. Append unrelated trailing data after the table to prove no EOF-equality rule. Corrupt either pair independently while main stays sane. Mutate the table after startup validation before actual deferred init; require a contained init/request error.

```cpp
QVERIFY(Mdx::btreeTestOldOrBad(twoSources, badFirstVolumeIndex));
QVERIFY(Mdx::btreeTestOldOrBad(twoSources, badSecondVolumeIndex));
QVERIFY(!Mdx::btreeTestOldOrBad(oneMainSource, zeroVolumeIndex));
```

Use a source-file list of three entries for the two-volume positives; fixture variable names in actual code must reflect that count, avoiding an off-by-one test oracle.

- [ ] **Step 2: Run RED proving both parsers are exercised**

Compile real MDX translation unit and inject data through its narrow adapter. Observe the deferred-init production parser separately from the guard so fixing only startup cannot satisfy the post-guard mutation case. A copied table parser in fixtures is not coverage.

- [ ] **Step 3: Implement range-checked name-plus-pair iteration**

Prove `dictFiles.size()>=1` before subtracting, compare counts in uint64. For nonzero count prove `tableOffset<=S` and `count<=(S-tableOffset)/12` before iteration; zero count/source agreement stays legal. At each position prove/read four-byte filename length, reserve eight bytes for pair, prove nameLength<=remaining-8, check container/API representability, read/skip with explicit length, then read m/root via memcpy/read. Never assume NUL termination or require the table ends at EOF. Validate each pair against the MAIN INDEX, under its file mutex; the predicate restores the parser's logical cursor. Reuse these exact checks in deferred init before vector indexing/source-name matching and opening each `IndexedMdd`. Do not index the parsed volume vector based only on source count. Every bad record/pair rebuilds at startup; later mutations set initError and reach request error. Catch open/header/table errors in old-or-bad.

- [ ] **Step 4: Run GREEN all MDX table and owning-file controls**

Run T34/T35 MDX rows, T36/T37, T26 position restoration and T42/T43/T49/T50 probes. Verify no stale parsed-vector indexing, no name terminator scan, no `_MDD` source-file predicate, and a good second record remains visible after the first root probe. Preserve original count/parser/folding/version expressions.

- [ ] **Step 5: Commit**

```bash
git add mdx.cc
git commit -m "fix(mdict): validate every persisted volume pair and table record"
```

## Task 16: Commit actual primary, secondary, and MDD-table guard coverage

**Implementer tier:** Advanced
**Lane:** lane-tests
**Owns:** tests/**
**Depends on:** 11, 15

**Files:**
- Modify: `tests/tst_btree_index/{tst_btree_index.pro,tst_btree_index.cc,fixtures.hh,fixtures.cc,owner_adapters.hh}`.

**Interfaces:**
- Consumes actual guard forwards from Tasks 12–15: one path argument for SoundDir/Stardict/Xdxf/Sdict/Aard/Bgl/DictdFiles/Lsa/ZipSounds/Epwing/Zim/Slob, `(path,bool hasZipFile)` for Dsl/Gls, `(dictFiles,path)` for Mdx, public `FtsHelpers::ftsIndexIsOldOrBad(path,BtreeDictionary*)`. Census forwards consume actual owner/header parsers, not test copies.
- Implements T03 `foreign_sounddir_header_is_not_sane`, T34 `every_primary_producer_routes_bad_info_to_rebuild`, T35 `every_secondary_tree_routes_bad_info_to_rebuild`, T36 `mdd_table_parsing_is_bounded`, T37 `zip_secondary_pair_uses_owning_index_file`.

- [ ] **Step 1: Write required row inventories and concrete assertions first**

T34 data has exactly these 16 owners: SoundDir, Stardict, Dsl, Gls, Xdxf, Sdict, Aard, Bgl, DictdFiles, Lsa, ZipSounds, Zim, Slob, Epwing, Mdx, FtsHelpers. Each invokes its real guard with valid/unchanged-identity bad main pair/open failure/short header. T35 covers FOUR optional ZIP owners, ZIM and SLOB required resources, two distinct MDD volumes, and separate FTS; each corrupts one pair with main valid and includes valid/absence/empty-tree controls. T36 includes every table boundary and actual deferred init; T37 differentiates same-offset bytes in owning index versus source ZIP/MDD and both ZipSounds opens.

```cpp
QCOMPARE(primaryOwnerNames.size(), qsizetype(16));
QCOMPARE(QSet<QString>(primaryOwnerNames.cbegin(), primaryOwnerNames.cend()), expectedOwnerNames);
QVERIFY(actualGuard(badPairFixture));
QVERIFY(!actualGuard(validFixture));
```

This uses Qt 6's iterator-based `QSet<QString>` constructor. `actualGuard` is a typed dispatch to the listed forwards; it never calls the shared predicate in place of an owning guard. T03 uses the exact seven-word foreign header and separate shared-predicate check, plus a valid local longer-than-header file.

- [ ] **Step 2: Run RED for unregistered owner rows and prove a secondary mutation**

Initially require row inventory completeness before wiring new fixtures/closures. After the suite builds and baseline passes, disable only DSL's ZIP-pair predicate in a disposable copy: the exact T35 DSL row must fail while the valid/main pair controls still pass. Build failure is not a killed control. Task 24 automates this again as acceptance.

- [ ] **Step 3: Link every actual owner and construct hermetic byte fixtures**

Link the 16 real producer translation units plus their actual parser closures: `dsl_details.cc`, `bgl_babylon.cc`, `dictzip.c`, `encoding.cc`, `iconv.cc`, `htmlescape.cc`, `audiolink.cc`, `filetype.cc`, `langcoder.cc`, `xdxf2html.cc`, `ufile.cc`, `decompress.cc`, `ripemd.cc`, and feature dependencies demanded by the actual link. Follow application conditionals for EPWING (`epwing_book.cc`, `epwing_charmap.cc`, `-leb`), ZIM (`-llzma`, zstd), LZO, bzip2, TIFF and vorbis; do not add an external dictionary corpus. Use section GC to drop unrelated UI entry points, not stub called readers. Fixtures write actual native header layouts with original identity constants; test-only owner fixture injection may use actual IdxHeader definitions in owner TUs instead of duplicating a parser. Preserve the new aggregate target's QCoreApplication runtime even if dependencies link QtWidgets/WebEngine libraries.

- [ ] **Step 4: Run GREEN all rows and spec's producer command**

`step s6 every_primary_producer_routes_bad_info_to_rebuild every_secondary_tree_routes_bad_info_to_rebuild`; additionally run T03/T36/T37 and prior suite. Run feature-enabled configuration for EPWING/ZIM rows and assert compiled feature macros, not silent skips. Run eventual s2 selection now, including real T03. Count unique exercised primary rows and log every secondary pair/file identity.

- [ ] **Step 5: Commit**

```bash
git add tests/tst_btree_index
git commit -m "test(indexes): exercise every owning rebuild guard and pair"
```

## Task 17: Contain synchronous sound, ZipSounds, Dictd, and LSA requests

**Implementer tier:** Frontier
**Lane:** lane-producers
**Owns:** sounddir.cc, zipsounds.cc, dictdfiles.cc, lsa.cc
**Depends on:** 15, 16, 8, 9

**Files:**
- Modify: `sounddir.cc:137-300,318-379`; `zipsounds.cc:186-394`; `dictdfiles.cc:276-425`; `lsa.cc:108-163,224-329,389-507`.
- Disposable probes: corresponding T46 `chunk_record_consumers_check_each_field`, T47 `resource_workers_contain_lengths_and_redirect_cycles`, T48 `remaining_worker_closure_handles_empty_and_short_data` rows. Committed slots: Task 23.

**Interfaces:**
- Consumes `findArticlesChecked(wstring const&,bool=false)` from Task 1; `Reader::getBlock(uint32_t,vector<char>&)` from Task 8; both `IndexedZip::loadFile` overloads and false/empty/missing distinction from Task 9; Task 16's real-owner fixture linkage.
- Preserve synchronous `getArticle/getResource` signatures. Corruption returns `new Dictionary::DataRequestInstant(QString errorString)`; normal absence returns the existing false instant request and valid empty content may be a successful request with no bytes. Add `<Owner>::btreeTestWorker(WorkerCase const&):WorkerResult` for SoundDir/ZipSounds/DictdFiles/Lsa under the globally declared test interface.

- [ ] **Step 1: Write byte-suffix and source/WAV RED rows**

For SoundDir require bounded filename terminator and test zero/short/unterminated chunk, valid empty file and normal missing path separately. ZipSounds record prefixes are uint16 nameLength + nameLength bytes + uint32 ZIP offset: place each required field at the final 0..k-1 suffix bytes, then exact success, oversized name, malformed referenced ZIP and valid empty ZIP. Dictd empty source/article result must not copy from an empty vector; corrupt source-read sizes produce errors. LSA includes short Entry fields, impossible source/sample offset, sampleLength multiplication overflow/API narrowing, header-only zero-sample WAV and exact nonzero WAV. Failures must be error requests, not thrown exceptions or ordinary misses.

```cpp
BtreeTest::WorkerResult r = ZipSounds::btreeTestWorker(shortZipOffsetCase);
QVERIFY(!r.error.isEmpty());
QVERIFY(!r.success);
QCOMPARE(SoundDir::btreeTestWorker(validEmptySoundCase).data.size(), qsizetype(0));
```

- [ ] **Step 2: Run RED against each real synchronous API**

Use the disposable extended T46/T47/T48 rows with actual owner constructors/readers. Assert their returned requests are already finished and inspect real error/data state; a catch around the test itself must not hide an exception escaping the API. Preserve assertion-enabled baseline empty-result child crashes separately.

- [ ] **Step 3: Add dominating record proofs and outer request boundaries**

At each chunk consumer reject an empty buffer before a non-empty record; establish the returned pointer's integer offset inside its owning buffer, then require field width <= size-offset before every copy. Use bounded memchr or checked explicit length for SoundDir filenames. Never evaluate chunk.front/back before non-empty proof. Skip empty file reads/results/UI copies entirely. ZipSounds proves the uint16 field, its name bytes and uint32 offset in order with memcpy for unaligned values; a known referenced `loadFile==false` is corruption and returns error, while missing lookup remains absence.

Wrap the ENTIRE synchronous article/resource work, including initial checked lookup and alternative lookups, in std::exception/catch-all returning a non-empty error instant request. Dictd preserves its existing catch boundary but reports failure instead of a false success; validate source read length and returned length before copying. LSA validates source fields/ranges, widens all sample/byte/rate products before multiplication, checks vector/API/WAV field representability, and requires output >= sizeof(WavHeader) before writing. Prefer a local WavHeader copied with memcpy after proof; zero PCM still has a legitimate header. Make vorbis cleanup exception-safe without changing decoded semantics. Missing resources/cancellation retain prior behavior.

- [ ] **Step 4: Run GREEN every new row and decoder regressions**

Run all corresponding T46/T47/T48 probes plus T20/T38–T40/T03/T34–T37. Verify malformed referenced ZIP yields a request error, valid empty/large resources succeed, and no return path leaks a source handle or accesses empty data. Compile actual sounds/vorbis/ZIP owners with packaged flags.

- [ ] **Step 5: Commit**

```bash
git add sounddir.cc zipsounds.cc dictdfiles.cc lsa.cc
git commit -m "fix(workers): contain sound dictd and wav record failures"
```

## Task 18: Close Stardict, DSL, GLS, and XDXF field and resource boundaries

**Implementer tier:** Frontier
**Lane:** lane-producers
**Owns:** stardict.cc, dsl.cc, gls.cc, xdxf.cc
**Depends on:** 17

**Files:**
- Modify: `stardict.cc:350-390,880-990,1410-1450,1710-1750`; `dsl.cc:420-454,550-590,1380-1420,1910-2050`.
- Modify: `gls.cc:680-720,1010-1050,1200-1235,1280-1325,1420-1460`; `xdxf.cc:235-275,365-395,495-535,645-690,1020-1080`.
- Disposable probes: four owners' T46 `chunk_record_consumers_check_each_field` rows and T47 resource/empty-image rows; committed in Task 23.

**Interfaces:**
- Consumes checked btree lookup, actual chunk suffix contract and `IndexedZip` independent false/missing contract from Tasks 1/8/9, and real owner fixture types from Task 16.
- Preserve all article/headword/resource APIs; reader failures propagate to an owning request rather than being swallowed by logging-only inner catches. Test-only `<Owner>::btreeTestWorker(WorkerCase const&):WorkerResult` forwards invoke real record readers and request paths.

- [ ] **Step 1: Write each field-boundary RED matrix before repairs**

Stardict: two uint32 article properties plus following bounded headword; each binary article field length before advance; `loadString(0)` and zero output. DSL: two uint32 properties in both article/getArticleText paths, abbreviation count and every key/value length, zero image/data. GLS: two properties, empty lookup/headword vector before front, zero article/result. XDXF: all 0..8-byte suffixes for the nine-byte type/offset/size prefix, sized description/name, abbreviation tables. All resource requests include known corrupt IndexedZip entries and valid empty/large content; every resource catch must set error, not only log.

```cpp
for (int suffix = 0; suffix < 9; ++suffix) {
  auto c = makeXdxfSuffixCase(suffix);
  QVERIFY(!Xdxf::btreeTestWorker(c).error.isEmpty());
}
QVERIFY(Xdxf::btreeTestWorker(exactNineBytePrefixCase).success);
```

The generator's exact-size control includes a valid source range so a different downstream error cannot mask the record proof.

- [ ] **Step 2: Run RED actual article, ArticleText, headword and resource routes**

Extend disposable rows to call all four real translation units through forwards. Require both direct reader exceptions and request error/completion evidence as specified per row. Initial checked lookup must be inside the boundary; tests must include corruption before any inner rendering try block.

- [ ] **Step 3: Implement per-field proofs and full request catches**

Use integer cursor and `need(k)`-style checks based on remaining validated bytes before each fixed read, length prefix or skip; a whole-chunk bound is insufficient. `loadString(0)` returns an empty string without `front()`. Preserve zero-length article/image/transcoding success paths by avoiding empty-buffer data pointers/copies. Bound each binary field using subtraction, not an overflowing end offset. In XDXF do not form a pointer from chunk.front before proving non-empty/nine-byte suffix.

Switch all error-sensitive/alternative/resource lookups to `findArticlesChecked`. Enclose complete resource/article/headword worker bodies in std/catch-all boundaries; error strings non-empty, partial matches/data cleared under dataMutex, exactly normal single finish, cancellation and runnable semaphore retirement preserved. Synchronous resources return error instant requests. Where `getArticleText` currently logs storage failure and returns apparent empty success, let storage exceptions reach the existing owning FTS/request catch; do not add a shared last-error state. Keep ordinary missing files distinct from malformed referenced ZIP resources. Add narrow forwards only, no copied readers.

- [ ] **Step 4: Run GREEN four-owner rows and positive boundaries**

Run all T46/T47 rows for these owners, include ArticleText and headword routes, exact/zero/large positives and malformed initial lookup. Re-run their T34/T35/T37 guard rows and T40/T41 decoder probes. Compile optional TIFF/SVG/transcoding paths with package feature conditions and verify no zero-data path invokes GUI-only initialization in the pure test process.

- [ ] **Step 5: Commit**

```bash
git add stardict.cc dsl.cc gls.cc xdxf.cc
git commit -m "fix(workers): bound lexicon fields and resource requests"
```

## Task 19: Repair BGL's parallel resource decoder and Aard/SDict whole requests

**Implementer tier:** Frontier
**Lane:** lane-producers
**Owns:** bgl.cc, aard.cc, sdict.cc
**Depends on:** 18

**Files:**
- Modify: `bgl.cc:330-420,561-605,748-910,1011-1140`; `aard.cc:430-470,540-590,695-835`; `sdict.cc:370-420,525-570,680-735`.
- Disposable probes: BGL T46 field rows, BGL T47 `resource_workers_contain_lengths_and_redirect_cycles`, Aard/SDict T48 `remaining_worker_closure_handles_empty_and_short_data`. Committed slots: Task 23.

**Interfaces:**
- Consumes Task 8's actual-output/empty-chunk contract, checked btree lookup and existing source File/codec APIs.
- Keep real BGL resource-list/decoder ownership in `bgl.cc`, not a test/ChunkedStorage replacement. Expose Bgl/Aard/Sdict `btreeTestWorker(WorkerCase const&):WorkerResult` forwards to actual consumers; failure propagation uses existing requests.

- [ ] **Step 1: Write RED all independent BGL and source-size routes**

BGL article/headword/metadata: separately truncate/miss each headword/displayed-headword/article/description NUL; oversized icon/metadata length; storage exception in headword worker. BGL resources: filename length and following offset crossing EOF, truncated U/C frame, C past source, zlib checksum/tail/U mismatch and valid empty/large output. Aard/SDict: short source length fields, length beyond file or API range, zero article/resource/result, and corruption in initial lookup outside original inner catches.

```cpp
QVERIFY(!Bgl::btreeTestWorker(badResourceFrame).error.isEmpty());
QVERIFY(Bgl::btreeTestWorker(emptyResourceFrameWithRealStream).success);
QVERIFY(!Aard::btreeTestWorker(initialLookupCorruptCase).error.isEmpty());
QVERIFY(!Sdict::btreeTestWorker(sourceLengthPastEofCase).error.isEmpty());
```

- [ ] **Step 2: Run RED real parallel decompressor and outer workers**

Do not satisfy BGL resource tests through ChunkedStorage, which does not own that path. Observe actual resource decoder entry, request finish/error and exit. Run original empty-vector regressions only in bounded children.

- [ ] **Step 3: Implement file/chunk field proofs and request-wide containment**

For BGL bound all four strings separately, icon/metadata sizes within actual suffix and each resource-list filename/following offset before allocation. Resource frame proves file header/C range then uses incremental zlib with wide 1032 bound, exact U/C, RAII end and actual-output publication; U=0 with real stream is a legal resource, C=0 is not. No fixed resource-size cap. Add missing std/catch-all over entire resource/headword/article run, not just UTF-8 catches; errors clear success-only data and finish once with semaphore retirement.

For Aard/SDict, bound source record/decompression sizes against non-negative file size and receiving API types before resize/read, skip empty-vector read/result copies, and use checked lookup inside the outer request catch. Preserve actual codecs and normal miss/cancel semantics. A decoder returning a valid empty result is not justification for a later front/copy.

- [ ] **Step 4: Run GREEN independent resource and source boundary matrices**

Run all corresponding T46–T48 rows, core/chunk decoder slots, and these producers' T34 rows. Include valid empty/large BGL resources, empty Aard/SDict article results and exact-boundary successes; verify non-empty error and one completion for each malformed case. Check actual packaged codec/configuration builds.

- [ ] **Step 5: Commit**

```bash
git add bgl.cc aard.cc sdict.cc
git commit -m "fix(workers): harden bgl resources and source sized requests"
```

## Task 20: Prove EPWING fixed-field records and contain both text paths

**Implementer tier:** Frontier
**Lane:** lane-producers
**Owns:** epwing.cc
**Depends on:** 19

**Files:**
- Modify: `epwing.cc:285-320,390-430,475-525,630-665,710-785`.
- Disposable probes: EPWING T46 `chunk_record_consumers_check_each_field` and relevant T48 empty-copy rows; committed in Task 23.

**Interfaces:**
- Consumes checked lookup and `ChunkedStorage::Reader::getBlock` with explicit suffix checks. Preserve EPWING's declared page/offset field widths and `NO_EPWING_SUPPORT` conditional, not a guessed two-uint32 layout.
- Expose `Epwing::btreeTestWorker(WorkerCase const&):WorkerResult` only in the enabled actual EPWING TU. The disabled configuration asserts the feature is absent, not that the enabled row passed.

- [ ] **Step 1: Write byte-boundary EPWING RED rows**

Read actual page/offset declarations, compute `k=sizeof(page)+sizeof(offset)`, and generate each final suffix 0..k-1 plus exact k valid record. Exercise both article-text readers, real article/headword request routes and zero output copies. Use synthetic source/fixture injection at the real record-reader seam, not an external dictionary corpus or a replacement decoder.

```cpp
QVERIFY_EXCEPTION_THROWN(readActualEpwingRecord(shortSuffix), Dictionary::Ex);
QVERIFY(Epwing::btreeTestWorker(exactRecordCase).success);
QVERIFY(!Epwing::btreeTestWorker(shortArticleCase).error.isEmpty());
```

`readActualEpwingRecord` is a narrow forward to the actual reader, with exact exception type asserted from the implemented production contract; no decoder body resides in the test.

- [ ] **Step 2: Run RED with EPWING enabled**

Build the real epwing/epwing_book/epwing_charmap closure and `-leb`; require short-record expectations to fail at runtime before repair. A no_epwing_support build is not RED/coverage for this task.

- [ ] **Step 3: Add declared-width suffix proofs and full request reporting**

Prove every page/offset field before memcpy in both article and ArticleText paths; use unaligned-safe copies, checked source decoder inputs and skip zero output copies. Expand outer worker catches to include initial checked lookup/record decode and set non-empty error, clear success-only state under dataMutex, preserve one finish/cancel/runnable exit. Let direct ArticleText decoding storage failures propagate to its owning request boundary rather than become apparent successful empty text. Do not edit `epwing_book.*` outside this lane's allowlist; if a newly reachable hit requires such a change, raise the allowlist conflict with source evidence instead of silently skipping it.

- [ ] **Step 4: Run GREEN enabled rows and disabled compile closure**

Run all EPWING T46/T48 probe cases and T34 guard row in the enabled build, exact-width and zero controls included. Compile `CONFIG+=no_epwing_support` separately to verify no test/adapter reference leaks across the feature boundary. Record enabled feature evidence explicitly.

- [ ] **Step 5: Commit**

```bash
git add epwing.cc
git commit -m "fix(epwing): validate record fields before article reads"
```

## Task 21: Bound ZIM clusters, SLOB bins, and multi-target redirects

**Implementer tier:** Frontier
**Lane:** lane-producers
**Owns:** zim.cc, slob.cc
**Depends on:** 20

**Files:**
- Modify: `zim.cc:420-460,570-660,1310-1360,1520-1620`; `slob.cc:430-475,565-615,1410-1465,1600-1705`.
- Disposable probes: ZIM/SLOB T48 `remaining_worker_closure_handles_empty_and_short_data`; committed in Task 23.

**Interfaces:**
- Consumes checked lookup, required-resource inert-open/guard behavior from Task 14, actual ZIM/SLOB codecs and source readers.
- Preserve actual 4/8-byte ZIM offset choice, SLOB widths/codecs and public request interfaces. Expose `Zim/Slob::btreeTestWorker(WorkerCase const&):WorkerResult` forwards for real readers AND article/resource requests.

- [ ] **Step 1: Write RED short-cluster/bin and redirect matrices**

ZIM cases: 0..3/0..7 bytes before first cluster offset, first offset too small/large, nondivisible table, selected pair short, decreasing blob offsets, end beyond actual decompressed cluster, two-target redirect cycle, zero result/resource. SLOB cases: fewer than four bytes (pin unsigned underflow), offset-table product too large, bin index outside table, each short four-byte field, length beyond bin and exact/zero controls. Redirect self/two-target cases must terminate normally with request errors within a 5-second child deadline.

```cpp
QVERIFY(!Zim::btreeTestWorker(twoTargetRedirectCase).error.isEmpty());
QVERIFY(!Slob::btreeTestWorker(threeByteBinCase).error.isEmpty());
QVERIFY(Zim::btreeTestWorker(exactEmptyBlobCase).success);
QVERIFY(Slob::btreeTestWorker(exactEmptyBinCase).success);
```

- [ ] **Step 2: Run RED feature-enabled actual readers/requests**

Enable MAKE_ZIM_SUPPORT through `CONFIG+=zim_support`; link lzma/zstd as the application does. Prove actual cluster/bin reader entry and source-codec route before request error. Missing feature/compile errors cannot be counted as protected behavior.

- [ ] **Step 3: Implement offset-table and actual-output range proofs**

ZIM proves first-width before any offset read, then first offset range/divisibility/count, selected offset pair, ordering, and blob end within actual decompressed cluster with uint64/subtraction. No pointer/substring before proof. Replace direct-self-only redirect check with a local visited-target set on each logical source redirect; no arbitrary retry budget. SLOB proves whole offset table, bin index and every four-byte field before subtracting/multiplying; never subtract four from an unsigned shorter buffer. Verify bin data length before slicing.

Enclose initial checked lookup and all article/resource decode in std/catch-all request boundaries, set errors/clear partial successes/finish once/preserve cancellation and runnable retirement. Skip zero result/image/resource copies. Keep valid empty blobs/bins distinct from absent names; required empty resource roots still validate.

- [ ] **Step 4: Run GREEN all matrices and enabled/disabled builds**

Run these T48 rows with exact/empty positives, all redirect cycles and sane no-match controls; run ZIM/SLOB T34/T35 guard rows. Compile package ZIM-enabled and default ZIM-disabled builds separately, retaining assertions. Verify no source-format/version or resource absence convention changed.

- [ ] **Step 5: Commit**

```bash
git add zim.cc slob.cc
git commit -m "fix(workers): bound zim clusters slob bins and redirects"
```

## Task 22: Terminate MDX resource redirects and surface all FTS storage failures

**Implementer tier:** Frontier
**Lane:** lane-producers
**Owns:** mdx.cc, ftshelpers.cc
**Depends on:** 21

**Files:**
- Modify: `mdx.cc:613-722,791-921,1170-1260`; `ftshelpers.cc:890-960,1220-1343`.
- Disposable probes: T45 `fts_worker_reports_storage_failures`, T46 FTS posting readers, T47 `resource_workers_contain_lengths_and_redirect_cycles`, plus T42/T43/T49/T50 regressions. Committed slots: Task 23.

**Interfaces:**
- Consumes `findArticlesChecked(wstring const&,bool=false)`, `Reader::getBlock`, Task 10's real RecordInfo/source/metadata proof and Task 14's FTS guard/mutex/version behavior.
- Preserve FTS request constructors/public API and existing MDX cache/resource interfaces. Expose `FtsHelpers::btreeTestWorker(WorkerCase const&):WorkerResult` for real FTS workers/posting readers; extend `Mdx::btreeTestWorker` from Task 10 with cached and asynchronous redirect routes. For FTS combined/whole-word selection, use the real request's existing search-mode arguments via a test fixture variant field declared in Task 5 (`QString variant`), not test-only decoding.

- [ ] **Step 1: Write RED redirects, inert FTS and posting-reader matrices**

MDX: empty resource name, malformed/odd/truncated UTF-16LE redirect prefix, empty target, self/two-target `@@@LINK=` in both MddResourceRequest and cached getCachedFileName paths, plus valid empty/large resource and source map/slice cases. FTS: invalid pair, sane root/corrupt descendant, chunk too short for count, count>remaining/4, unaligned offsets, whole-word and combined-index paths; each has non-empty error, no partial success, one finish and runnable exit. Include normal no-match/cancellation and ensure corrupt indexes never silently fall back to full scan.

```cpp
auto r = FtsHelpers::btreeTestWorker(combinedPostingOverflowCase);
QVERIFY(!r.error.isEmpty());
QVERIFY(!r.success);
QCOMPARE(r.finishedSignals, 1);
QVERIFY(r.runnableExited);
```

- [ ] **Step 2: Run RED with actual request paths**

Use existing real MDX/FTS closures and the disposable probe. Ensure FTS corruption reaches checked lookup or posting reader rather than being satisfied by an empty legacy result. Observe both MDX redirect loops independently; killing a hung baseline child is RED only, not a final pass.

- [ ] **Step 3: Implement target-set termination and checked FTS reads/catches**

Guard resource-name indexing before name[0]. Prove complete redirect prefix, even UTF-16LE length/terminator and nonempty target before decoding; enter each logical target into a request-local set and report repeats instead of `continue` on the same target. Cached-resource route uses the same rule and reports through the owning request, not an unbounded cache resolver. Keep source record proofs from Task 10 intact and include initial lookup/alternative-target lookups in outer catches.

For EVERY FTS posting-list reader prove four-byte count and `uint64_t(count)<=remaining/4` before vector allocation/read; use memcpy for unaligned offsets. Use `findArticlesChecked` for FTS lookup and any dictionary retrieval where the catching API would hide corruption. A checked traversal of inert FTS must produce an error, not a scan fallback. Extend std/catch-all over the whole FTS run, setErrorString, clear success-only data under dataMutex, finish once on the normal completion path, retain cancellation and semaphore release. Ensure inner logging catches do not convert storage errors into success.

- [ ] **Step 4: Run GREEN worker and spec s7 selections**

Run all T45–T47 probes plus `step s7 mdx_stylesheet_records_are_bounded mdx_description_is_bounded mdx_article_record_slice_is_bounded mdd_record_slice_is_bounded fts_worker_reports_storage_failures prefix_worker_reports_error_and_finishes` against the extended disposable suite. Verify one completion and runnable retirement for errors/cancellation, both MDX redirect consumers, both FTS posting paths and actual error text. Retain every benign empty/no-match control.

- [ ] **Step 5: Commit**

```bash
git add mdx.cc ftshelpers.cc
git commit -m "fix(workers): terminate mdict redirects and report fts corruption"
```

## Task 23: Commit real worker-closure and ZIP owner-error acceptance slots

**Implementer tier:** Advanced
**Lane:** lane-tests
**Owns:** tests/**
**Depends on:** 16, 22

**Files:**
- Modify: `tests/tst_btree_index/{tst_btree_index.pro,tst_btree_index.cc,fixtures.hh,fixtures.cc,owner_adapters.hh}`.

**Interfaces:**
- Consumes all real `<Owner>::btreeTestWorker(WorkerCase const&):WorkerResult` forwards from Tasks 10/17–22; WorkerCase/WorkerResult fields are defined in Task 5. FTS variant selects existing production modes; no shadow worker logic.
- Implements the complete T41 `indexed_zip_deflate_is_incremental_and_exact`, T45 `fts_worker_reports_storage_failures`, T46 `chunk_record_consumers_check_each_field`, T47 `resource_workers_contain_lengths_and_redirect_cycles`, T48 `remaining_worker_closure_handles_empty_and_short_data`. Extends T42/T43/T44/T49/T50 only with additional rows/observations; do not create duplicate top-level slots.

- [ ] **Step 1: Write all required owner/route matrices before fixture extensions**

T46 rows MUST include SoundDir, ZipSounds, Stardict, DSL, GLS, XDXF, BGL, EPWING and FTS: each fixed prefix's 0..k-1 suffixes, exact success, oversized following lengths, missing terminators and legal zero content. T47 invokes actual BGL resource list/decompressor, sound records and BOTH MDX/MDD redirect routes; malformed filename/resource sizes, missing fields, empty/self/two-target redirects, malformed compressed data plus empty/large positives. T48 includes Aard, SDict, Dictd, LSA, ZIM, SLOB AND downstream image/result-copy paths: empty outputs, short WAV/source/cluster/bin records, overflowed products and exact controls. T45 covers invalid FTS pair, descendant, both posting paths and no fallback. T41 includes Task 9's full raw-deflate matrix and a REAL owning resource request on malformed referenced data.

```cpp
BtreeTest::WorkerResult r = invokeActualOwner(c);
QVERIFY2(!r.error.isEmpty(), qPrintable(caseName));
QVERIFY(!r.success);
if (isAsynchronousRoute(c)) { QCOMPARE(r.finishedSignals, 1); QVERIFY(r.runnableExited); }
```

`invokeActualOwner` is the fixture's switch to actual forwards; it does not catch exceptions to fabricate WorkerResult for request routes. Synchronous instant requests are observed as already finished; do not invent an emitted-signal count for a signal emitted before construction returns.

- [ ] **Step 2: Run RED for missing rows, then mechanism-specific runtime controls**

Fail the expected-owner/route inventory while fixtures are missing. After all fixtures link/baseline passes, route FTS through legacy catching lookup in a disposable copy and require T45 runtime failure. Disable one fixed-prefix check and require the corresponding byte-boundary row to fail (sanitizer crash is a failed mutant test, not an accepted baseline). Keep original production RED evidence from each owner task.

- [ ] **Step 3: Implement hermetic owner/source fixtures and exact top-level inventory**

Use real dictionary/parser objects with small temp files, real source compression and test-only injection into actual state/readers; no external corpus. Wire observation before scheduling, process events/wait with deadlines, await runnable retirement before owner/file destruction. Link actual image/transcoding/codec paths as required; avoid GUI-only fixture constructors, not production record checks. Define each normative T01–T50 slot exactly once in the QTest class; `_data` providers/child dispatch/harness self-checks are not feature slots. Compare the 50 spec names mechanically to declarations/definitions and log complete data-row inventories, including enabled EPWING/ZIM and supported LZO rows.

- [ ] **Step 4: Run GREEN all 50 slots and feature-selected row inventories**

Build/run the complete standalone suite out of tree (no slot arguments), then each new T41/T45–T48 selection explicitly. Run spec s2/s3/s4/s6/s7 selections, adding all omitted normative rows. Verify every malformed request finishes with error and normal child exit, every positive empty/exact/large case succeeds, no required row skips, and all runnable semaphore checks complete. Run installed-feature and disabled-feature compile variants without reusing objects.

- [ ] **Step 5: Commit**

```bash
git add tests/tst_btree_index
git commit -m "test(workers): cover real record consumers and error completion"
```

## Task 24: Automate all seven disposable runtime negative controls

**Implementer tier:** Advanced
**Lane:** lane-tests
**Owns:** tests/**
**Depends on:** 23

**Files:**
- Create: `tests/tst_btree_index/negative_controls.py`, `tests/tst_btree_index/negative_controls.json`.
- Modify: `tests/tst_btree_index/fixtures.cc` only if a fixture needs a more discriminating runtime assertion; keep the 50 top-level names unchanged.

**Interfaces:**
- Consumes final production/test sources and standalone `tst_btree_index` target; the driver builds copies via the exact same qmake/compiler flags as its clean baseline and runs named slots with QTest XML output.
- Produces CLI `python3 tests/tst_btree_index/negative_controls.py --source "$GD_SRC" --build-root "$GD_BUILD_ROOT/negative-controls" --report-dir "$PM_RUN_ROOT/reports/btree-negative-controls"`; report JSON records source digest, baseline commands/results, each patch/changed path, build exit, slot/data-tag, exact intended failed assertion/source line, runtime exit and cleanup. Seven control IDs `NC01`..`NC07` are mandatory.

- [ ] **Step 1: Write a failing manifest-completeness/driver self-check**

Require seven unique IDs and exact expected slot mappings below; reject empty/missing/repeated controls, zero-match/multi-match mutation anchors, source paths outside the disposable copy, build failures counted as kills and runtime failures unrelated to the asserted slot/data-tag. Self-check uses small temporary fake command results (not a fake production test): all-green mutant, compile error, QTest runtime failure in wrong slot, timeout/crash, and intended runtime assertion. The first four must be REJECTED; only the named assertion is accepted. Verify cleanup on raised assertions and interrupted runs. Put this self-check in the driver, not a 51st QTest feature slot.

- [ ] **Step 2: Run RED harness rejection tests**

Run `python3 tests/tst_btree_index/negative_controls.py --self-test` before implementing its decision/cleanup logic; observe the intended failing validation. Never run mutations in the integration source checkout or retain a failed copy as the baseline of the next mutation.

- [ ] **Step 3: Implement fresh-copy baseline/build/runtime checks for every control**

For each ID create a new absolute temp source outside `$GD_SRC`, `cp -a` then remove copied `.git`, create a separate external output dir, hash relevant sources, run pristine named tests successfully, apply exactly one mechanism mutation and build successfully before running the same named tests. Do not mutate a prior mutant or change tests/fixtures in the mutant. QTest XML/log must show the intended expectation failed; no timeout, process signal, compile/link failure or incidental initialization error counts as a killed control. Preserve logs/patches under the PM report root, remove temp source/build trees in finally, and verify the integration checkout source hashes/dirty set remain unchanged.

| Control | Mutation applied ONLY in the disposable source | Required runtime detection |
|---|---|---|
| NC01 | Remove only the non-empty-next-leaf rejection in `readNextLeaf`, retaining framing/type checks and normal success behavior | T22 `next_link_to_empty_leaf_throws` must fail the named empty-target expectation in a helper/route data row. Choose the low-level helper assertion requiring `exCorruptedChainData` before a chain pointer is returned, so the mutant returns without dereferencing `data()+4` and the counted kill is a QTest assertion, not an ASan crash. Exact non-empty positive controls stay green |
| NC02 | Bypass only `findChainOffsetExactOrPrefix`'s next-leaf `readNextLeaf` call using the old local decode/trailer transition, leaving the helper itself and other advances intact | T21 `next_leaf_internal_node_from_prefix_walk_throws` must fail its expected-corruption-exception assertion on the exact `word00001z` gap lookup. Keep post-root padding and do not call `readChain` on the returned mutant pointer; a rename/link failure or a matching-prefix-only test does not count |
| NC03 | Omit ONLY DSL's optional ZIP-pair predicate from its actual `indexIsOldOrBad` guard, preserving main-pair and identity checks | T35 `every_secondary_tree_routes_bad_info_to_rebuild` DSL-zip data row must fail `QVERIFY(oldOrBad)`, with valid main/identity and readable malformed secondary root. Valid/absent pairs and other secondary rows stay green |
| NC04 | Add back the rejected `U <= 65536` chunk restriction in the actual chunk record acceptance decision, without changing writer bytes or fixtures | T39 `writer_oversized_chunk_block_reads_back` must fail its actual 65,537-byte production-writer readback assertion/expected-success boundary. Catch the mutant's decoder exception at the test observation boundary and turn it into the named QTest failure, rather than allowing an unhandled process termination |
| NC05 | Add an explicit 4,096-visited-node limit to the actual per-traversal entry decision while retaining repeated-offset rejection | T33 `bulk_scan_more_than_4096_leaves_succeeds` must fail its actual 4,100-leaf result/expected-success assertion. Convert the injected cap's exception into that QTest assertion; a compile error or mere source-name mutation does not prove the cap was exercised |
| NC06 | Replace only FTS's error-sensitive `findArticlesChecked` call with legacy catching `findArticles`, preserving all other request behavior | T45 `fts_worker_reports_storage_failures` must fail the invalid-pair/descendant non-empty-error assertion (silent no-match or fallback is not acceptable), with benign no-match control unchanged |
| NC07 | Skip only the per-record inner-chain walk during node shape validation, leaving nonzero chainSize and exact outer framing checks intact | T12 `truncated_inner_chain_record_throws` must fail its expected `exFailedToDecompressNode` assertion on the outer-valid 12-byte leaf with `chainSize=4` (plus missing-terminator/short-offset rows). Call `readNode`, not a later string parser, so the mutant wrongly accepts without an out-of-bounds scan |

These seven IDs match the FINAL normative table in spec §11.2, not its illustrative textual replacement anchors; weakening the predicate, disabling cycle detection, or removing MDX slice checks are optional extra probes and cannot replace any required control. Make mutation anchors from actual final function bodies, assert match counts and retain exact patches; do not guess fixed line numbers, rename declarations to cause build failures, or silently broaden patches.

- [ ] **Step 4: Run GREEN baseline and all seven intended kills**

Run self-check and the complete driver CLI. Require baseline all green, seven successful mutant builds, seven intended runtime-assertion detections with exactly the seven mappings above, and no survivors. Rerun standalone baseline after mutations; compare source hashes/dirty set. Failure to kill any selected control blocks completion; do not edit production here to force a result.

- [ ] **Step 5: Commit**

```bash
git add tests/tst_btree_index/negative_controls.py tests/tst_btree_index/negative_controls.json tests/tst_btree_index/fixtures.cc
git commit -m "test(indexes): automate seven isolated runtime negative controls"
```

## Task 25: Enforce the full out-of-tree, sanitizer, and retained-feature gates

**Implementer tier:** Advanced
**Lane:** lane-tests
**Owns:** tests/**
**Depends on:** 24

**Files:**
- Create: `tests/tst_btree_index/run-gates.sh`.
- Modify: `tests/tst_btree_index/tst_btree_index.pro`, `tests/tests.pro`, `tests/testsupport.pri` ONLY where required to propagate flags/features or keep shadow outputs external; preserve existing suite behavior.

**Interfaces:**
- Consumes final 50-slot suite, exact 19-project aggregate, requiredEnvironment/watchdog conventions and negative-control driver; no new production API.
- Produces CLI `GD_BUILD_ROOT=<new absolute temp> PM_RUN_ROOT=<existing run root> bash tests/tst_btree_index/run-gates.sh` recording source/flags/feature macros/commands/exits and logs under `$PM_RUN_ROOT/reports/btree-gates/`. It must preserve inherited CXXFLAGS and assertions, use fresh outputs per configuration and fail if a required enabled row skipped.

- [ ] **Step 1: Write failing gate-command/inventory self-checks**

Require the driver contains s1..s9 stages, commands below, all 50 slot names from spec, 19 aggregate projects, separate default/retained/enabled/sanitizer output roots and runtime log status propagation under pipefail. Reject accidental source-tree object/MOC/UI/RCC/Makefile outputs and a gate claiming success after a subcommand failed. A command-plan validation mode must first fail until the driver is implemented; it does not substitute for actually running gates.

- [ ] **Step 2: Run RED self-check and establish actual clean standalone baseline**

Use source snapshot/hash/dirty-set capture BEFORE builds; do not modify/reset pre-existing user changes. The plan-only integration checkout has no generated output. Run standalone assertion-enabled suite once before gate harness refinements to separate harness/config bugs from production failures. Diagnose environmental failures explicitly rather than relaxing tests.

- [ ] **Step 3: Implement every spec §11.1 command with external output paths**

Keep `set -euo pipefail`, qmake6/make/QT_QPA_PLATFORM=xcb/xvfb-run/timeout 900 invocations, real source `.pro` paths and `-j"$(nproc)"`. Preserve all base compiler flags including `_GLIBCXX_ASSERTIONS`; log actual compiler and feature defines. These are separate runs, not one warmed Makefile:

```bash
GD_SRC=$(git rev-parse --show-toplevel)
: "${GD_BUILD_ROOT:?absolute external build root}"
mkdir -p "$GD_BUILD_ROOT/standalone" "$GD_BUILD_ROOT/sanitizers" \
         "$GD_BUILD_ROOT/all" "$GD_BUILD_ROOT/retained" "$GD_BUILD_ROOT/features"
qmake6 "$GD_SRC/tests/tst_btree_index/tst_btree_index.pro" \
  -o "$GD_BUILD_ROOT/standalone/Makefile"
make -C "$GD_BUILD_ROOT/standalone" -j"$(nproc)"
QT_QPA_PLATFORM=xcb xvfb-run -a timeout 900 "$GD_BUILD_ROOT/standalone/tst_btree_index"

qmake6 "$GD_SRC/tests/tst_btree_index/tst_btree_index.pro" \
  -o "$GD_BUILD_ROOT/sanitizers/Makefile" \
  'QMAKE_CXXFLAGS+=-fsanitize=address,undefined -fno-omit-frame-pointer -g' \
  'QMAKE_LFLAGS+=-fsanitize=address,undefined'
make -C "$GD_BUILD_ROOT/sanitizers" -j"$(nproc)"
ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
  QT_QPA_PLATFORM=xcb xvfb-run -a timeout 900 "$GD_BUILD_ROOT/sanitizers/tst_btree_index"

qmake6 "$GD_SRC/tests/tests.pro" -o "$GD_BUILD_ROOT/all/Makefile"
make -C "$GD_BUILD_ROOT/all" -j"$(nproc)"
QT_QPA_PLATFORM=xcb xvfb-run -a timeout 900 make -C "$GD_BUILD_ROOT/all" check

qmake6 "$GD_SRC/tests/tst_btree_index/tst_btree_index.pro" \
  -o "$GD_BUILD_ROOT/retained/Makefile" 'CONFIG+=no_ffmpeg_player'
make -C "$GD_BUILD_ROOT/retained" -j"$(nproc)"
QT_QPA_PLATFORM=xcb xvfb-run -a timeout 900 "$GD_BUILD_ROOT/retained/tst_btree_index"

qmake6 "$GD_SRC/tests/tst_btree_index/tst_btree_index.pro" \
  -o "$GD_BUILD_ROOT/features/Makefile" 'CONFIG+=zim_support'
make -C "$GD_BUILD_ROOT/features" -j"$(nproc)"
QT_QPA_PLATFORM=xcb xvfb-run -a timeout 900 "$GD_BUILD_ROOT/features/tst_btree_index"
```

Also run the entire 19-suite aggregate in a fresh retained `CONFIG+=no_ffmpeg_player` output, not just the standalone target, and separate compile/run targets with `CONFIG+=no_epwing_support`, `CONFIG+=no_extra_tiff_handler`, and default ZIM disabled. Ensure the test `.pro` actually propagates retained options/feature macros into real producer dependency closures; root application `goldendict.pro` is read-only in this plan. An existing `GD_COMPILE_CLOSURE=1` archive can diagnose changed-source dependencies but cannot replace either the 19-suite check or a linked application executable.

Build the REAL application twice, once with the package's enabled features/assertions and once with the retained disabled options. Source-local quoted `.moc` includes and `version.txt` generation make an ordinary root shadow build unreliable; use fresh disposable source/build copies OUTSIDE the checkout, with source-local outputs confined to those temporary copies. Never invoke the root `check` target against the integration checkout: its recipe writes to source `tests/`. Read Arch's configured flags from the existing build environment/configuration, record their provenance, and pass them explicitly through qmake rather than assuming `$CXXFLAGS` is imported. The configured assertion flag must be present; do not replace all package flags with only `-D_GLIBCXX_ASSERTIONS`.

```bash
: "${GD_ARCH_CXXFLAGS:?actual configured Arch CXXFLAGS, including _GLIBCXX_ASSERTIONS}"
: "${GD_ARCH_CFLAGS:?actual configured Arch CFLAGS}"
: "${GD_ARCH_LDFLAGS:?actual configured Arch LDFLAGS}"
case "$GD_ARCH_CXXFLAGS" in *-D_GLIBCXX_ASSERTIONS*) ;; *) exit 1 ;; esac
for cfg in package retained; do
  app="$GD_BUILD_ROOT/application-$cfg"
  test ! -e "$app"
  mkdir -p "$app"
  cp -a "$GD_SRC/." "$app/"
  rm -rf "$app/.git"
  opts=(PREFIX=/usr)
  if test "$cfg" = package; then
    opts+=('CONFIG+=chinese_conversion_support' 'CONFIG+=zim_support')
  else
    opts+=('CONFIG+=no_ffmpeg_player' 'CONFIG+=no_epwing_support' \
           'CONFIG+=no_extra_tiff_handler')
  fi
  (cd "$app" && qmake6 goldendict.pro "${opts[@]}" \
      "QMAKE_CXXFLAGS+=$GD_ARCH_CXXFLAGS" \
      "QMAKE_CFLAGS+=$GD_ARCH_CFLAGS" \
      "QMAKE_LFLAGS+=$GD_ARCH_LDFLAGS" && make -j"$(nproc)")
  test -x "$app/goldendict"
done
```

Keep both real linked executables and build identities for Task 27, not just a compile archive. The package variant includes ZIM, EPWING, LZO, extra TIFF, Chinese conversion and the default media backend; the retained variant proves their guarded off paths. Apply the same actual Arch flags to the suite/aggregate qmake calls above; inspect the emitted compiler/link lines and configuration probes. Resolve quoted `.moc` shadow-build dependencies in tests-owned qmake rules/INCLUDEPATH without generating files in the checkout or mixing prior MOC/object directories. Application optional-feature libraries must mirror the package; enabled EPWING/LZO rows cannot be silently skipped.

Run named incremental stages s1..s7 from Global Constraints/earlier tasks as well as the whole suite; stage s8 is standalone + sanitizer + real application builds and s9 is aggregate + retained/features + census/scratch-lookup/diff handoff (Tasks 26–27). Store a structured stage status, never infer s9 acceptance before both Tasks 26–27 evidence exists. No sanitizer suppression should hide the new readers; any platform/toolchain limitation is an explicit blocker, not a green run.

- [ ] **Step 4: Run GREEN all real gates and compare clean-source snapshots**

Execute the driver, seven negative controls and full 19-suite aggregate with 900-second check timeout. Inspect every exit/log and actual 50-slot/row count; assertions, ASan/UBSan all pass, no unsupported required fixture skips. Verify no build outputs appeared in checkout and only allowed tests files changed. A failing unrelated suite blocks a global pass unless independently demonstrated and explicitly escalated; do not repair unrelated production outside ownership.

- [ ] **Step 5: Commit**

```bash
git add tests/tst_btree_index/run-gates.sh tests/tst_btree_index/tst_btree_index.pro tests/tests.pro tests/testsupport.pri
git commit -m "test(indexes): gate shadow builds sanitizers and feature variants"
```

## Task 26: Publish a complete reachable-hit inventory and local index census

**Implementer tier:** Frontier
**Lane:** lane-tests
**Owns:** tests/**
**Depends on:** 25

**Files:**
- Create: `tests/tst_btree_index/closure_inventory.json`, `tests/tst_btree_index/audit_closure.py`, `tests/tst_btree_index/corpus_sweep.py`.
- Modify: `tests/tst_btree_index/{tst_btree_index.cc,fixtures.hh,fixtures.cc,owner_adapters.hh,run-gates.sh}` for test-executable census dispatch and final verification.
- Reports (NOT staged/owned source): `$PM_RUN_ROOT/reports/worker-sweep.md`, `btree-corpus-sweep.txt`, `btree-final-diff.txt` and machine-readable companions.

**Interfaces:**
- Consumes actual `btreeTestPairs` census forwards from Tasks 12–15, final guard return values, 50 test slots, actual closure source files and s1..s9/negative-control evidence.
- Test executable CLI `tst_btree_index --btree-pair-census <manifest.json>` bypasses QTest only for that explicit mode, uses QCoreApplication and actual owner/FTS guard + pair readers, emits one JSON object per file/pair. Manifest entries specify owner, absolute index path, source paths, hasZipFile and FTS dictionary-version context; unknown/ambiguous ownership is reported unresolved, never guessed by file length.
- Audit CLI `python3 tests/tst_btree_index/audit_closure.py --source "$GD_SRC" --inventory tests/tst_btree_index/closure_inventory.json --report "$PM_RUN_ROOT/reports/worker-sweep.md"`; census CLI `python3 tests/tst_btree_index/corpus_sweep.py --binary "$GD_BUILD_ROOT/standalone/tst_btree_index" --manifest "$GD_INDEX_MANIFEST" --report "$PM_RUN_ROOT/reports/btree-corpus-sweep.txt"`. Both use production-reader evidence, never rewrite a local corpus/index.

- [ ] **Step 1: Write failing inventory/census completeness checks**

Seed the inventory with EVERY §7.2 row below, then enumerate every direct/indirect reachable `front`, `back`, fixed-width memcpy, unbounded string scan, indexed byte and source-length allocation hit. Each record has `path`, enclosing function, current hit anchor+line, call route/helpers, consuming field/width, dominating proof/empty branch or explicit non-reachability reason, test slot/data-tag, feature configuration and evidence. Render `worker-sweep.md` with the EXACT spec §7 columns: `file:line`, `operation` (`front`/`back`/`[0]`/`strlen`/`memcpy`/`resize`/`exit`/`abort`/`catch`), `owning request`, `reachable from worker` (yes/no + citation), `bounds proof or repair`, `named test`, `feature configuration`, and `disposition`; no blank cell is allowed. No aggregate counts/unexplained ignored hits. Make the checker fail on a newly inserted unsafe copy in a disposable source fixture, removed proof reference, missing field row, or required feature row marked skipped. Census self-check includes all 16 owners, all secondary kinds, MDD multiple volumes, inaccessible file, foreign/invalid pair and owner/source ambiguity; fixture census only is not a substitute for local-data reporting.

| Inventory row | Mandatory reachable scope (follow helpers, not just the starting lines) |
|---|---|
| Core | `btreeidx.cc` leaf/internal decode, chain enumeration, findChainOffset successor, prefix worker and bulk/FTS helpers |
| Chunk framing | `chunkedstorage.cc` offset table, selected page/block, zlib decode and empty output |
| ZIP helper | `indexedzip.cc` central/local headers, names/comments/extras, source payload/raw inflater, both overloads |
| SoundDir | filename chunks, article/article-text and synchronous resource paths, empty source files |
| ZipSounds | uint16 name length/name/uint32 ZIP offset, both inherited trees, actual resource load/error |
| Stardict | two properties, headword strings, binary field lengths, loadString(0), image/result/ZIP paths |
| DSL | article and ArticleText property pairs, abbreviation key/value fields, image/result/ZIP paths |
| GLS | property pair, empty headword chain, article/resource/result/ZIP paths |
| XDXF | nine-byte record, sized name/description/abbreviations, article/resource/result/ZIP paths |
| BGL metadata | four bounded strings, description/icon/metadata sizes, headword/article error and copies |
| BGL resources | independent resource-list name/offset plus compressed-frame decoder and empty resource |
| EPWING | actual declared page/offset widths in both readers, request and output copies, enabled config |
| MDX/MDD records | RecordInfo fixed record, source map range/null, actual decoded slice, both loaders and MDD table reparse |
| MDX metadata | stylesheet count/name/value fields and bounded description plus init/request propagation |
| MDX redirects | asynchronous MDD and cached-resource target loops, name[0]/UTF-16 prefix/length/termination |
| Mdict common | every ScopedMemMap caller and `parseCompressedBlock` envelope/plain/LZO/zlib exactness |
| FTS | shared guard, checked lookup, whole-word/combined posting readers, whole run/cancel/finish/semaphore |
| Aard | source length/decompression input/output, empty and request-wide checked lookup/catches |
| SDict | source length/decompression input/output, empty and request-wide checked lookup/catches |
| Dictd | source reads, zero outputs/copies, checked lookup and request errors |
| LSA | Entry source fields, wide sample/WAV products, header before write, empty PCM and errors |
| ZIM | 4/8-byte cluster offset table/blob ranges, redirect set, empty resources/results, enabled config |
| SLOB | four-byte fields/offset table/bin index, no unsigned subtract underflow, empty results |

Rows split the spec's grouped entries only to make independent decoders/redirects visible. They do not omit/relax any §7.2 scope. Emit zero/unreachable findings explicitly with call-site reasoning, not by excluding entire files. A helper reached outside lane-owned files requires proof of safety or a reported allowlist blocker; do not silently edit it.

- [ ] **Step 2: Run RED checker controls and literal source sweep**

Run inventory/census self-check before filling missing evidence. Execute the spec's literal sweep and save raw output alongside the inventory:

```bash
cd "$GD_SRC"
grep -R -n -E '\.front\(|\.back\(|strlen\(|memcpy\(|\[[[:space:]]*0[[:space:]]*\]' \
  btreeidx.cc chunkedstorage.cc indexedzip.cc sounddir.cc stardict.cc dsl.cc gls.cc \
  xdxf.cc sdict.cc aard.cc bgl.cc dictdfiles.cc lsa.cc zipsounds.cc epwing.cc \
  zim.cc slob.cc mdx.cc mdictparser.cc ftshelpers.cc \
  | tee "$PM_RUN_ROOT/reports/btree-worker-closure-raw.txt"
```

Also inspect `.data()/constData()`, iterator/vector indexing, implicit NUL string constructors and allocation/source reads not matched by that regex. Grep's status 1 for no hits is not a build failure; capture/report it accurately. Expected grep hits are not defects if their dominating proofs are documented. The inventory checker is an evidence-completeness tool, not a claim to mechanically prove C++ safety.

- [ ] **Step 3: Implement actual pair census, manual helper closure and corpus sweep**

Census reads only existing local indexes supplied through `GD_INDEX_MANIFEST` (derive the owner/source mapping from current dictionary configuration when available; do not print private contents). For each primary/resource/ZIP/MDD/FTS pair record owner/type, owning index path, persisted field names/m/root, absent-vs-present, file size/root range result, actual guard old-or-bad result and whether it reaches existing rebuild routing. Hash every participating file with SHA-256 before AND after the sweep, record file identity (path/device/inode/size/mtime), and require unchanged bytes/identity; a concurrent change is a failed observation, not evidence about a stable index. The reviewed corpus contains 20 top-level files: require 17 accepted and exactly three rejected foreign SoundDir files, with every valid embedded/FTS pair accepted. If the current inventory differs, emit an explicit per-file inventory delta and actual counts rather than forcing or silently dropping the historical expectation. The absent optional `{0,0}` pair is reported absent, not predicate-failed; required empty roots must be sane. MDX enumerates every actual variable record using the shared parser and main-file ownership; FTS retains dictionary version/mutex context. Failed/short/ambiguous files remain visible entries, not dropped. Never open in write mode or actually rebuild/delete user's indexes as part of the sweep. Foreign bad pairs must route to rebuild/error and valid large/empty indexes remain accepted.

Write the complete reviewed reachable-hit inventory with dominating proofs and actual slot/data-tag evidence from Task 23; manually follow source/mapping/codec/image helpers. Any unclosed hit is a BLOCKED finding owned by the correct production lane: stop/escalate rather than modify another lane's files in this task. If the local corpus is absent, report `ABSENT` with searched/configured scope and still run hermetic fixture census; do not claim the acceptance item was exercised. No available pair counts or unexplained path omissions.

- [ ] **Step 4: Run final acceptance, negative controls, and immutable-format diff audit**

Run all 50 slots, feature-enabled owner rows, assertions, ASan/UBSan, full 19-suite aggregate and retained options from Task 25; rerun seven controls from Task 24 after final test CLI changes. Run complete literal/manual closure sweep and local corpus census, preserving raw logs/structured results under PM reports. Produce `btree-final-diff.txt` from final `git diff`/history inspection: no signature, packed header layout, format/folding/parser version or builder serialization changes; no invented correctness caps; every §6 guard/secondary open and §7.2 row closed; no legacy error-swallowing request lookup; all edits confined to lane-owned files; no generated build artifacts. Record source/test digests and clean/dirty set before/after. Only claim the inventory/census portion complete when every required item is evidenced; Task 27 still owns scratch application lookup and the final post-lookup acceptance/diff decision. No-corpus/unsupported feature/unclosed helper is explicitly unresolved, not silently green.

- [ ] **Step 5: Commit**

```bash
git add tests/tst_btree_index/closure_inventory.json tests/tst_btree_index/audit_closure.py tests/tst_btree_index/corpus_sweep.py tests/tst_btree_index/tst_btree_index.cc tests/tst_btree_index/fixtures.hh tests/tst_btree_index/fixtures.cc tests/tst_btree_index/owner_adapters.hh tests/tst_btree_index/run-gates.sh
git commit -m "test(indexes): audit worker closure and persisted pair census"
```

## Task 27: Verify isolated application lookup, rebuild hashes, and final acceptance

**Implementer tier:** Frontier
**Lane:** lane-tests
**Owns:** tests/**
**Depends on:** 26

**Files:**
- Create: `tests/tst_btree_index/application_acceptance.py`.
- Modify: `tests/tst_btree_index/run-gates.sh` (s9 final dispatch/result collection).
- Read only: `config.cc:49-99,2349-2380,2416-2426,2464-2485`, `main.cc:471-479`, `sources.cc:128-149`, corpus configuration/indexes and the Task 25 executables.
- Reports only: `$PM_RUN_ROOT/reports/btree-application-acceptance.{json,md}`, scratch logs and final `btree-final-diff.txt`.

**Interfaces:**
- Consumes `corpus_sweep.py --binary <path> --manifest <json> --report <path>` and its per-file/pair/hash results, the actual `application-package/goldendict` executable from Task 25, and the complete T01–T50/gate/control/sweep reports.
- Produces `python3 tests/tst_btree_index/application_acceptance.py --binary "$GD_BUILD_ROOT/application-package/goldendict" --config "$GD_CONFIG_COPY_SOURCE" --manifest "$GD_INDEX_MANIFEST" --scratch-root "$GD_BUILD_ROOT/application-acceptance" --reports "$PM_RUN_ROOT/reports"`. This is a separate local-corpus acceptance, not another feature slot or a fake application. Required inputs absent or unsupported acceptance routes produce explicit BLOCKED status, never success.

- [ ] **Step 1: Write scratch isolation and acceptance assertion checks first**

Write `--self-test` cases using temporary config/index files and command-result fixtures, covering attempted writes outside scratch, symlinks resolving to active index directories, source mutation during copy, a missing foreign file, a changed supposedly-good hash, process crash/timeout, false success without a sound lookup, and a portable startup that never loads SoundDir. Require every case to be rejected and active sentinel hashes unchanged. A normal live window alone is not rebuild evidence. The actual acceptance assertions are: exactly the historical three foreign SoundDir files change to native six-field headers with sane roots; the other 17 top-level files remain byte-identical; all embedded/FTS valid pairs stay accepted; `hello` plus one present source sound word per affected dictionary yields actual successful article/resource results; process remains live until deliberate graceful close, with no new process core. A different corpus must have the explicit Task 26 inventory delta and matching per-file expectations.

- [ ] **Step 2: Run RED on missing driver and audit the portable precondition**

Run the self-test before implementing isolation/result logic. Re-read the actual supported portable detection and configuration behavior: `portable/` beside the executable selects portable mode, content is `<applicationDir>/content`, and indexes belong to `<applicationDir>/portable/index`. There is no invented `--config` flag. Confirm `main.cc:477` clears `cfg.soundDirs` and `sources.cc:135-147` disables portable sound directories. Record this as `PORTABLE_SOUNDDIR_UNREACHABLE` with source citations and a reproducible pristine scratch launch, not an alleged failure of the new guard. Under the immutable ownership/spec constraints this is a concrete contract blocker: return it to the PM/controller for spec or allowlist resolution before counting the portable SoundDir gate green. Do not implement an environment-variable workaround, binary patch, preloader, or out-of-allowlist source edit. An isolated nonportable scratch-HOME diagnostic can show the rebuild path works but cannot discharge the portable acceptance without an explicit approved disposition.

- [ ] **Step 3: Implement supported scratch layout, snapshots, and real-process observations**

Use Python `pathlib`/`tempfile`, `hashlib.sha256`, `shutil.copy2/copytree`, `xml.etree.ElementTree` and `subprocess.Popen` with argument arrays. Reject a pre-existing/nonexternal scratch root and any writable scratch path resolving outside it. Copy config/index bytes, never hardlink or writable-symlink the active index/config/cache. Capture source hashes before/after copying and verify copied identities before launching. Configure isolated HOME, XDG_CONFIG_HOME, XDG_CACHE_HOME, XDG_DATA_HOME, XDG_RUNTIME_DIR (0700), private D-Bus and Xvfb; copy the real executable into a unique scratch executable path so QtSingleApplication cannot send the test lookup to the user's running process. Copy/provide application data under scratch `content/`, translating source paths deliberately and recording the mapping; active dictionary source files may only be read, never rewritten. The actual launch shape, once the blocker has an approved resolution, is:

```bash
# application_acceptance.py copies the verified config+index into a scratch HOME and
# copies the executable to a unique path first (so QtSingleApplication cannot forward
# the lookup to the user's running process).
SCRATCH_HOME="$GD_BUILD_ROOT/application-acceptance/home"
mkdir -p "$SCRATCH_HOME/.goldendict" "$SCRATCH_HOME/.cache" "$SCRATCH_HOME/.config"
HOME="$SCRATCH_HOME" XDG_CONFIG_HOME="$SCRATCH_HOME/.config" \
XDG_CACHE_HOME="$SCRATCH_HOME/.cache" XDG_RUNTIME_DIR="$SCRATCH_HOME/.run" \
QT_QPA_PLATFORM=xcb dbus-run-session -- xvfb-run -a \
  "$GD_BUILD_ROOT/application-acceptance/bin/goldendict" --log-to-file hello
```

The driver manages this command with `Popen`, bounded startup/lookup polling, stdout/stderr capture and finally cleanup; it must not use `timeout`'s deliberate process kill as proof of normal application completion. Record actual article/resource success through a real local UI/resource observation or the existing production request adapter on the scratch-rebuilt owner, not only a header hash or a screenshot of a blank window. Keep the no-new-core assertion scoped to this child PID/time interval so unrelated desktop crashes do not invalidate or mask it. Close the scratch application normally and wait; a crash, unintended early exit, or timeout fails. Re-run the production pair census on scratch indexes after rebuild, check six native header fields using the real SoundDir reader plus root predicate, compare all unchanged-file hashes, and then rehash active inputs to prove no mutation. A missing corpus or unresolved portable blocker yields the named BLOCKED report with all collected evidence, not fabricated observations.

- [ ] **Step 4: Run GREEN self-tests and all required application/final gates**

Run driver self-tests, then the actual CLI against the available corpus and Task 25 executable after the portable contract has been resolved through the controller. Require all process/rebuild/lookup/hash/core assertions, 20/17/3 historical counts or an explicit inventory delta, and active bytes unchanged. Reconcile exact results from the 50-slot suite, 19-suite aggregate, retained/feature/application builds, ASan/UBSan, seven spec-mandated negative controls, complete `worker-sweep.md`, and pair/hash census. Refresh the final diff audit after all implementation commits: unchanged persisted versions/layout/wrappers and builder serialization; all 16 producers/secondary trees/workers accounted for; no generated checkout artifacts or out-of-lane files. Full acceptance is blocked if any required local run, feature, helper, or portable disposition is unresolved. Do not label the plan's syntactic validation or driver self-tests an implementation pass.

- [ ] **Step 5: Commit**

```bash
git add tests/tst_btree_index/application_acceptance.py tests/tst_btree_index/run-gates.sh
git commit -m "test(indexes): verify scratch application rebuild and lookup acceptance"
```


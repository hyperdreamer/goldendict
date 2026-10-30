# Btree index and its readers: no index byte is trusted, no worker can end the process

**Status:** design v5 for the continuation gate (v1 blocked by review 1, v2 by
review 2, v3 by the PM after review 3 died mid-review, v4 by review 4 — see
`reports/design-review-1.md` … `design-review-4.md` and
`design-review-4-disposition.md`)
**Topic:** `btree-index-foreign-header-abort`
**Date:** 2026-10-30

## Background

GoldenDict 2:1.5.1.r2828.g862b1a98-1 (this repository, Qt 6 build) aborts on
**every** word lookup. `SIGABRT` in a `QThreadPool` worker; three cores in
`coredumpctl`.

```
/usr/include/c++/16/bits/stl_vector.h:1336:
std::vector<unsigned char>::front() [with _Tp = unsigned char]:
  Assertion '!this->empty()' failed.

#6 BtreeIndex::readNode                    btreeidx.cc:490
#7 BtreeIndex::findChainOffsetExactOrPrefix btreeidx.cc:538
#8 BtreeWordSearchRequest::findMatches     btreeidx.cc:316
```

`btreeidx.cc:490` is `idxFile->read( &compressedData.front(), compressedData.size() )`
with `compressedSize == 0`. `_GLIBCXX_ASSERTIONS` is in Arch's default `CXXFLAGS`,
so an otherwise silent out-of-bounds read aborts the process.

The dictionary was identified at the `openIndex` boundary:

```
openIndex maxEl=159      root=908172     <- MDX dictionary (correct)
openIndex maxEl=2057158  root=378        <- MyTTS sound directory (nonsense)
```

`~/.goldendict/index/` is shared with other GoldenDict builds. Three
sound-directory indexes were written on 2026-05-26 by a different producer
(`goldendict-ng`) with a **28-byte / 7-field** header; this tree's
`SoundDir::IdxHeader` is **24 bytes / 6 fields**, and the producer-identity
fields were not bumped (both carry `signature = 0x58524453`,
`formatVersion = 11`). The reader consumes fields 0–5, so it takes
`indexBtreeMaxElements = 2057158` and `indexRootOffset = 378`, seeks into the
chunk area, and reads two zero size words.

Sound directories are queried for every word, so every query reaches the abort.
The 17 other indexes in that directory were verified structurally intact.

**Confirmed immediate mitigation (no code):** deleting those three index files
makes the *unpatched* installed binary rebuild them into the correct 24-byte
header and stop crashing — verified on an isolated copy of the real config and
indexes. That mitigation is not the fix: any future foreign or corrupt index
recurs, because nothing in the traversal distrusts the file.

## Goals

1. No index byte is trusted. Every offset, count and length the traversal later
   dereferences has been proven to lie inside the buffer or file region it came
   from, with checked wide arithmetic.
2. No path reachable from a search worker can terminate the process (assertion,
   `exit()`, `abort()`) or loop unboundedly. Failures surface as per-dictionary
   errors.
3. An index whose header cannot describe its own file is detected and rebuilt, so
   the affected dictionary heals itself — for **every** persisted
   `(file, IndexInfo)` pair, including secondary trees.
4. These properties belong to `BtreeIndexing::BtreeIndex` and
   `ChunkedStorage`, not to the discipline of sixteen producers.
5. No false positives: every index this build can legitimately write must still
   open and read, including large nodes. No needless rebuilds.

## Non-goals

- Supporting the foreign 28-byte sound-directory header; rebuilding is correct.
- Containing construction failures per format in `loaddictionaries.cc` (one
  `try/catch` around the whole load). Recorded follow-up, not this fix.
- Changing any on-disk format or bumping any format version.
- Build-time compression failures (`btreeidx.cc:1057/1069`, reached only by
  index construction, never by a search worker).
- Re-establishing *semantic* validity: separator sort order, or that a child
  offset points at a node of the right kind. Cycles are handled by a traversal
  budget (D6), not by tree proof.

## Architecture

| Layer | Property |
|---|---|
| D1 | `readNode()` proves the record and the node's **entire** content are in-bounds and well-formed before the bytes become traversable |
| D2 | one shared predicate decides whether an `IndexInfo` can describe a node of a given file |
| D3 | `openIndex()` is inert on failure; the object never traverses, and the failure is a per-dictionary error |
| D4 | every persisted `(file, IndexInfo)` pair is validated before use, so a bad one rebuilds |
| D5 | every process-terminating and empty-container read reachable from a worker is closed |
| D6 | traversal is budgeted, so a well-formed cycle cannot hang a worker |

### D1 — `readNode()`: record stage, then shape stage

**Record stage**, before any allocation or seek beyond the header:

- `offset` and the 8-byte record header lie inside the file: `offset <= size`
  and `8 <= size - offset`, in `uint64_t`;
- `uncompressedSize != 0` and `compressedSize != 0`;
- `compressedSize <= size - offset - 8` (no wrap);
- `uncompressedSize <= compressedSize * 1032 + 64` — the **provable** expansion
  limit of the deflate format (a 258-byte match is encoded in about two bits, so
  no zlib stream can expand by more than 1032:1). This replaces v3's
  `MaxNodeBytes = 256 MiB` constant, which review 2 correctly rejected as a
  false-positive machine: a legitimate builder-produced node was measured at
  19,500,008 bytes because entry count is bounded but the chain payload attached
  to an entry is not (design-review-2.md, BLOCKER 1). There is now **no magic
  number**: the bound is a property of the format, so it can never reject a
  stream the producer wrote.
- the payload is inflated **incrementally** (`inflate()` into `out`, growing in
  64 KiB steps) and abandoned with a throw as soon as `out.size()` would exceed
  that bound; the final size must equal `uncompressedSize`. Because `out` grows
  with the bytes actually produced, `uncompressedSize` is never trusted to size
  an allocation.

A `> 16 MiB` builder-generated node must read back successfully (test 16). The
`__BTREE_USE_LZO` branch is not compiled; if ever enabled it must carry an
equivalent guard, and that requirement is stated in the code comment.

Any violation throws `exFailedToDecompressNode`.

**Shape stage**, on the inflated buffer, `n = out.size()` and
`m = indexNodeSize`. Let `first` be the first `uint32_t` (`n >= 4` is required
first).

- Internal node (`first == 0xFFFFFFFF`):
  - `n >= 4 + (m + 1) * 4` — the child-offset array must fit. This closes the
    unsigned `leafEnd - ptr` underflow at `btreeidx.cc:611`.
  - the region `[4 + (m+1)*4, n)` must contain exactly `m` separators, each
    non-empty and NUL-terminated, consuming the remainder exactly (the last byte
    of the buffer is the final NUL, and no NUL is doubled). This bounds every
    `strlen()` and `next += strlen(next)+1` walk.
- Leaf (`first` is an entry count `e`):
  - `e <= m` — `buildBtreeNode()` only emits a leaf when the entry count fits
    `maxElements`.
  - walking from `ptr = 4`, for each of the `e` entries:
    `4 <= n - ptr`, read `chainSize`, then `chainSize <= n - ptr - 4`, and
    `ptr` advances by `4 + chainSize`. After the last entry `ptr == n` exactly.
  - **each chain payload must itself be proven well-formed**, because
    `readChain()` (`btreeidx.cc:855`) parses word NUL, prefix NUL, 4-byte
    offset with no bound of its own and validates only afterwards:
    - `chainSize != 0` — every chain the builder writes holds at least one
      record (minimum 7 bytes). A zero-size chain is rejected here, which also
      removes the empty-vector input that `findMatches()` indexes at
      `btreeidx.cc:330` (belt: D5.2 also guards that call site).
    - the payload must parse into whole records consuming exactly `chainSize`:
      a NUL-terminated word, a NUL-terminated prefix, then 4 bytes of
      `articleOffset`, repeated.
- `entryCount == 0` is legal **only** for an empty tree, whose root is then a
  single 4-byte leaf. `readNode()` cannot know whether it is reading the root, so
  it accepts the empty leaf; the traversal keeps its existing rule (an empty
  non-root leaf throws `exCorruptedChainData`) and the advance helper below
  rejects entryCount 0 outright, because an advance can only target a non-empty
  leaf.
- Nothing else is claimed: the separators need not be sorted, and a child offset
  need not point at a node of the right kind. Both are covered where they matter
  (D2's predicate reads the root; every child is read through this same
  validated `readNode`; cycles are bounded by D6).

### D2 — `BtreeIndex::indexInfoIsSane()`: one predicate, no recursion

```cpp
static bool indexInfoIsSane( IndexInfo const &, File::Class &, Mutex & );
```

Public static member (so it may use the protected `readNode()` and set a probe's
private members). Returns false when:

- `btreeMaxElements == 0` or `> BtreeMaxElements`;
- `rootOffset <= size` fails or `8 > size - rootOffset` (checked in `uint64_t`);
- the root record fails D1 — evaluated by constructing a probe `BtreeIndex`,
  setting `idxFile`/`idxFileMutex`/`indexNodeSize`/`rootOffset` **directly**, and
  calling `readNode()`.

**It must not call `openIndex()`.** v1 did, which made
`openIndex() → predicate → openIndex()` infinitely recursive (review 1, finding
1a).

Locking and position contract (review 2, MAJOR 5): the predicate acquires the
given mutex for the whole save/seek/read/restore window, acquires no other lock,
and restores the caller's file position on both the success and the failure
path. `openIndex()` may therefore call it while the caller already holds that
mutex (it is a `QRecursiveMutex`). Cost is one seek, two 4-byte reads and one
node inflate per index open.

### D3 — `openIndex()`: inert on failure, fully reset on every call

`openIndex()` is the only caller of the predicate. Contract (review 2, MAJOR 2
and MAJOR 4):

1. Reset to the inert state **first**: `indexNodeSize = 0`, `rootOffset = 0`,
   `rootNodeLoaded = false`, `rootNode.clear()`, `indexUsable = false`.
2. Set `idxFile` / `idxFileMutex` to the given file and mutex regardless of the
   outcome, so a failed open is reported as `exIndexInfoUnusable` and not
   confused with the never-opened state.
3. Evaluate the predicate; any exception from it (file I/O, bad node) is caught
   and leaves the object inert.
4. Only on success commit `indexNodeSize`, `rootOffset` and `indexUsable = true`.

`bool indexUsable = false;` is an **in-class initializer**, and the existing
constructor keeps `idxFile( 0 )`, so a default-constructed instance still reports
`exIndexWasNotOpened` through the existing `if ( !idxFile )` guard.

Every traversal entry point keeps that guard and adds, immediately after it,
`if ( !indexUsable ) throw exIndexInfoUnusable();`. The list, enumerated from
`btreeidx.hh`: `findArticles` (89), `findAllArticleLinks` (92),
`getAllHeadwords` (95), `findArticleLinks` (98), `getHeadwordsFromOffsets` (104),
`findChainOffsetExactOrPrefix` (122), `readChain` (134). `readNode()` needs no
flag: it is only reachable with an offset the already-guarded traversal
produced.

`exIndexInfoUnusable` is a new `DEF_EX` beside the other btree exceptions
(`"The index header does not describe this file"`), derived from
`Dictionary::Ex`.

**Public exception contract (review 2, MINOR 3).** The guards throw. The public
methods keep their existing catch policy: `findArticles()` catches
`std::exception`, logs `"Articles searching failed"` and returns an empty vector;
`BtreeWordSearchRequest` catches and reports a per-dictionary error. Tests
therefore assert the documented public behaviour (empty result plus logged
error) and exercise the throwing contract through a test-only subclass that
exposes the protected methods. No public method is claimed to propagate.

**Why inert rather than a constructor throw:** `LoadDictionaries::run()` wraps
the whole load loop in one `try/catch` (`loaddictionaries.cc:78–103`), so a
throw from a constructor would leave the user with no dictionaries at all.

### D4 — every persisted `(file, IndexInfo)` pair is validated

Each pair below is validated in the producer's existing old-or-bad guard, after
its signature/version checks, combined with `||` so an insane `IndexInfo` routes
into that format's existing rebuild branch.

Primary trees (one per producer, each producer's own guard function):

| Producer | File | Guard |
|---|---|---|
| `sounddir.cc` | `<indexdir>/<id>` | `indexIsOldOrBad` |
| `stardict.cc` | `<indexdir>/<id>` | `indexIsOldOrBad` |
| `dsl.cc`, `gls.cc`, `xdxf.cc` | `<indexdir>/<id>` | own |
| `sdict.cc`, `aard.cc`, `bgl.cc`, `dictdfiles.cc`, `lsa.cc`, `zipsounds.cc` | `<indexdir>/<id>` | own |
| `zim.cc`, `slob.cc`, `epwing.cc` | `<indexdir>/<id>` | own |
| `mdx.cc` | `<indexdir>/<id>` | `indexIsOldOrBad` |

Secondary trees — same file, separate `IndexInfo` pair, so the **owning
producer's** guard must validate each one explicitly (review 2, BLOCKER 3):

| Owner | Secondary tree | Where the pair lives | Required wiring |
|---|---|---|---|
| `mdx.cc` | each `.mdd` resource btree | `mddIndexInfos` records at `idxHeader.mddIndexInfosOffset` in the main file, opened at `mdx.cc:438–472` | extend `indexIsOldOrBad` (`mdx.cc:1377`) to parse every record and validate it against the main file; any failure ⇒ rebuild |
| `stardict.cc` | zip resource btree | `zipIndexBtreeMaxElements` / `zipIndexRootOffset` in the main header (`:150–152`, opened `:300`) | fold into the existing guard at `:297` |
| `dsl.cc`, `gls.cc`, `xdxf.cc` | zip resource btree | `zipIndexBtreeMaxElements` / `zipIndexRootOffset` in the main header (`dsl.cc:480`, `gls.cc:570`, `xdxf.cc:303`) | same |
| `ftshelpers.cc` | full-text index | separate `<indexdir>/<id>_FTS` file with its own `FtsIdxHeader` (`ftshelpers.hh:23–42`) | validate the FTS header's own pair against the **FTS file**, in `ftsIndexIsOldOrBad` |
| `zim.cc`, `slob.cc` | embedded resource indexes | producer header fields | same pattern |

`IndexedZip` and `IndexedMdd` reuse `BtreeIndex::openIndex`, so they inherit D3
automatically; D4 is what makes them *heal* rather than merely stay inert.

### D5 — remove every process-terminating and empty-container read in the worker path

One rule, applied at every site reachable from `BtreeWordSearchRequest` and
article/resource retrieval: **no `.front()`, `[0]`, or `memcpy` destination may
be derived from index-controlled sizes without a non-empty and an in-bounds
check.** Concrete sites:

| # | Site | Today | Fix |
|---|---|---|---|
| D5.1 | `btreeidx.cc:407` | `exit( 1 )` when a next-leaf link lands on an internal node | `throw exCorruptedChainData()` (inside the enclosing `try`) |
| D5.2 | `btreeidx.cc:330` | `chain[ 0 ]` on a possibly empty `readChain()` result | skip the chain when empty (defence for D1's rejection of zero-size chains) |
| D5.3 | `chunkedstorage.cc:135–155` | `chunk.resize(uncompressedSize)` then `&chunk.front()` / `&compressedData.front()` with no validation — a verbatim copy of the abort pattern | require non-zero sizes, `uncompressedSize <= ChunkMaxSize` (65536 — the format's own bound, since addresses encode a 16-bit offset), both `<= size - offset`; inflate with the same incremental ceiling; exact length; throw `exFailedToDecompressChunk` |
| D5.4 | `mdx.cc:180–181` | `result.resize(recordSize)` then `memcpy(&result.front(), decompressed.constData() + recordOffset, recordSize)` — empty vector when `recordSize == 0`, and an out-of-bounds **read** for a hostile `recordOffset` | require `recordOffset <= decompressed.size()` and `recordSize <= decompressed.size() - recordOffset`; skip the copy when `recordSize == 0` |
| D5.5 | `indexedzip.cc:74,88` | `data.resize(header.uncompressedSize)` from the zip entry header, then used unconditionally | bound against the compressed size/`ChunkMaxSize` and use the size-checked path |
| D5.6 | `btreeidx.cc:826–845` (`findChainOffsetExactOrPrefix`) | the advance-to-next-leaf branch loads the next node and **returns `&extLeaf.front() + 4` without checking that it is a leaf**; a trailer pointing at an internal node makes `readChain()` (`:855`) interpret the child-offset array as chain data and read past the buffer | advance through one shared helper (below) |
| D5.7 | `btreeidx.cc:386–410`, `1382–1400`, `1499–1516` | three more copies of the same advance, each re-implementing the leaf check (`exit(1)` at 407, throws at 1397/1514) | same shared helper |

**D5.6/D5.7 — one advance helper, four call sites.** A single **protected**
`readNextLeaf( uint32_t & nextLeaf, vector< char > & leaf, char const * & leafEnd )`:

1. charges the D6 budget;
2. reads the 4-byte trailer for the *following* link (a short read is already an
   `exReadError`, i.e. a contained per-dictionary error);
3. loads the target through the validated `readNode()`;
4. requires the loaded record to be a **non-empty leaf**:
   `first != 0xFFFFFFFF` **and** `1 <= first <= indexNodeSize`; anything else
   throws `exCorruptedChainData`;
5. only then returns `leaf.data() + sizeof( uint32_t )` as the chain pointer.

Step 4's lower bound is mandatory and was missing from v4: `readNode()` accepts
the 4-byte empty leaf that a legal empty tree produces, and returning
`leaf.data() + 4` for it points exactly **past** the buffer, so `readChain()`
(`btreeidx.cc:861`) would `memcpy` four bytes out of bounds. The existing
empty-leaf rule protects the root/first-descent paths but not these four advance
paths (design-review-4.md, BLOCKER 1).

The helper is `protected`, not `private`: `BtreeWordSearchRequest::findMatches()`
is a separate class that reaches it through `BtreeDictionary &` exactly as it
reaches the inherited protected `readNode()` today, and friendship of
`BtreeDictionary` (`btreeidx.hh:212`) does not extend to a base's **private**
members (design-review-4.md, MAJOR 1). The implementation must compile the real
`findMatches()` call site, not only a test subclass.

Every advance site uses the helper, so the leaf check exists once and
`findMatches`'s `exit(1)` disappears with it. This is the one place where a
well-formed *internal* node would otherwise reach `readChain()` — a write path v3
missed.

The verification plan re-derives this inventory with a grep over the worker
closure so the list is auditable rather than asserted.

### D6 — traversal is budgeted

A validly framed internal node may point at itself or form a longer cycle; shape
validation cannot see that (review 2, MAJOR 1). Each traversal therefore carries
a budget: at most `MaxTraversalNodes = 4096` node reads **and** leaf advances per
request, after which it throws `exCorruptedChainData`. A btree with fan-out up to
`BtreeMaxElements` (8192) needs at most depth 4 to address 2³² words, so 4096 is
unreachable for a well-formed index and still terminates any cycle.

The budget is charged inside the shared `readNextLeaf()` helper (D5.6) and in the
node-descent loops of `findChainOffsetExactOrPrefix()`, `findArticleLinks()` and
`getHeadwordsFromOffsets()`, so every loop that can revisit an offset is bounded.

## Error handling matrix

| Condition | Before | After |
|---|---|---|
| Node points into non-node data (zero sizes) | `SIGABRT` | `exFailedToDecompressNode` |
| `compressedSize` past EOF / `uncompressedSize` past ceiling | multi-GB allocation | `exFailedToDecompressNode` |
| `uncompressedSize < 4` | OOB `uint32_t` read | `exFailedToDecompressNode` |
| Internal node shorter than its child array | unsigned underflow → `strlen()` outside the buffer | `exFailedToDecompressNode` |
| Unterminated separator | `strlen()` past the buffer | `exFailedToDecompressNode` |
| Leaf `count` absurd / framing not exact | huge `vector` + walk past buffer | `exFailedToDecompressNode` |
| Chain stub (`chainSize` 0 or non-exact records) | `readChain` overread / `chain[0]` abort | `exFailedToDecompressNode` (and D5.2) |
| Next-leaf link lands on an internal node | `exit(1)` from `findMatches`; **silent out-of-bounds read** from `findChainOffsetExactOrPrefix` | `exCorruptedChainData` from the shared helper at all four sites |
| Child offset cycle | hang | `exCorruptedChainData` after the budget |
| Zero-size chunk record | `SIGABRT` at `chunkedstorage.cc:146` | `exFailedToDecompressChunk` |
| `.mdd`/zip/`_FTS` record bogus | OOB / inert forever | producer rebuilds (D4) |
| Header `IndexInfo` cannot describe the file | accepted → every lookup aborts | rebuilt at startup (D4); else inert (D3) |
| Valid index, including a 19.5 MB node | works | works |

## Compatibility

- No format version bump, no on-disk format change.
- Valid indexes are accepted, not rebuilt. The user's `~/.goldendict/index` holds
  20 top-level files: **17 must be accepted, exactly the 3 foreign
  sound-directory files rejected**; plus every embedded `.mdd`/zip resource tree
  and every `*_FTS` file must be accepted. Any legitimate node up to
  `MaxNodeBytes` must read.
- `indexUsable` defaults to false and `idxFile` to null, so no existing
  default-construction path changes behaviour.

## Test strategy

New suite `tests/tst_btree_index/` (`TARGET = tst_btree_index`), a
`QCoreApplication` test: no display, no dictionary data, no Chromium. Links
`btreeidx.cc`, `chunkedstorage.cc`, `file.cc`, `dictionary.cc`, `mutex.cc`,
`utf8.cc`, `wstring.cc`, `wstring_qt.cc`, plus what `testsupport.pri` provides.
`tests/tests.pro`'s closed inventory grows 18 → 19.

Fixtures are built with `BtreeIndexing::buildIndex()` into a temp dir and then
mutated byte-wise. A test-only `BtreeIndex` subclass exposes the protected
`readNode` / `findChainOffsetExactOrPrefix` / `readChain`.

| # | Test | Pins |
|---|---|---|
| 1 | `builds_and_reads_back` | sane index: predicate true, opens, `findArticles` returns the chain |
| 2 | `open_index_terminates` | `openIndex` on a sane index returns and sets usable (v1's recursion would hang) |
| 3 | `foreign_sounddir_header_is_not_sane` | the exact foreign 6 words ⇒ predicate false |
| 4 | `insane_index_info_is_inert` | `openIndex` stores nothing; `findArticles` returns empty and logs; protected entry throws `exIndexInfoUnusable` |
| 5 | `zeroed_node_record_throws` | the original abort ⇒ `exFailedToDecompressNode` |
| 6 | `short_node_record_throws` | `uncompressedSize` 1–3 ⇒ throw |
| 7 | `truncated_node_offsets_array_throws` | internal node shorter than `4+(m+1)*4` ⇒ throw |
| 8 | `unterminated_separator_throws` | missing final NUL / doubled NUL ⇒ throw |
| 9 | `absurd_leaf_entry_count_throws` | `count` ≫ `m` in a small buffer ⇒ throw, no large allocation |
| 10 | `leaf_framing_not_exact_throws` | chain sizes that over/under-consume ⇒ throw |
| 11 | `zero_size_chain_throws` | `chainSize == 0` ⇒ throw (and `findMatches` never indexes `[0]`) |
| 12 | `truncated_inner_chain_record_throws` | word/prefix/offset not fitting the chain ⇒ throw |
| 13 | `out_of_range_node_offset_throws` | offset past EOF ⇒ throw, no allocation |
| 14 | `oversized_btree_size_rejected` | `btreeMaxElements > BtreeMaxElements` ⇒ predicate false |
| 15 | `max_node_ceiling_rejects_before_allocation` | hostile `uncompressedSize` above the ceiling ⇒ throw before the allocation |
| 16 | `builder_large_node_reads_back` | builder-produced node **> 16 MiB** must open and return all links (review 2, BLOCKER 1) |
| 17 | `next_leaf_internal_node_throws_not_exit` | leaf trailer rewritten to the internal root; a query that lands **after the last chain of a leaf** forces the advance ⇒ `exCorruptedChainData`, test process alive. Fixture: `buildIndex()` writes children first and the internal root **last**, so a bare builder file ends at the root's payload; the test must **append ≥ 4 readable bytes after the root** (or target a non-final internal node), otherwise the pre-fix path dies on a short trailer read and never reaches the leaf-type check (design-review-4.md, MAJOR 2). Start from a real matching chain so `findMatches` itself performs the advance |
| 18 | `self_child_node_cycle_throws` | internal root pointing at itself ⇒ `exCorruptedChainData` within the budget, tight timeout |
| 19 | `two_node_cycle_throws` | two internal nodes pointing at each other ⇒ same |
| 20 | `zero_size_chunk_throws` | chunk record `{0,0}` ⇒ `exFailedToDecompressChunk`, no abort |
| 20a | `next_leaf_internal_node_from_prefix_walk_throws` | same corrupt trailer, reached through `findChainOffsetExactOrPrefix`'s own advance. Fixture: a 130-word builder tree with the exact gap query `word00001z` (between the first leaf's last chain `word00001` and the next separator `word00002`), plus the same post-root padding; pre-fix the protected lookup must return a pointer into the internal node, post-fix it must throw `exCorruptedChainData` before `readChain` runs (D5.6) |
| 20b | `next_link_to_empty_leaf_throws` | trailer rewritten to an otherwise valid **empty** leaf (`entryCount == 0`); asserts `exCorruptedChainData` from the helper **and**, under ASan, no `readChain()` entry — the v4 `leaf.data()+4` past-the-end read. One case per advance path: prefix walk, `findMatches`, `findArticleLinks`, `getHeadwordsFromOffsets` |
| 21 | `reopen_valid_then_invalid_then_valid` | state is inert after an invalid open, usable again after a valid one, `rootNode` cleared each time |
| 22 | `default_constructed_is_unopened` | default instance reports `exIndexWasNotOpened`, not `exIndexInfoUnusable` |
| 23 | `every_entry_point_guarded_after_invalid_open` | each of the seven guarded entries reports the unusable state |
| 24 | `shared_file_position_restored_and_no_deadlock` | two threads: one traverses, one validates on the same file/mutex; no deadlock, position restored, results correct |

Tests 5–12 and 17–20 are the crash pins. Because
`assertions`-enabled libstdc++ is what turns these into kills, the meaningful
gate is the Arch-flags build (verification step 2); the suite is also run under
AddressSanitizer/UBSan as a **separate** standalone invocation, never inside the
aggregate, so a pre-fix demonstration cannot take down the gate.

End-to-end check (documented, not part of `make check` — needs Chromium and real
data): copy the real config + indexes to a scratch `HOME`, run
`goldendict hello`, require no `SIGABRT`, require the three foreign files
rewritten to the 24-byte header, and require the other 17 byte-identical.

## Verification plan

1. `make -C tests check` under `QT_QPA_PLATFORM=xcb xvfb-run`: all 19 suites.
2. Full build with Arch's own flags (PKGBUILD path) — the only configuration
   that reproduces the abort.
3. Standalone ASan/UBSan run of the new suite, replaying the review-2 probes
   (19.5 MB node, 12-byte leaf with `chainSize = 4`, `chainSize = 0` node,
   self-child cycle, `{0,0}` chunk) to show the reads are gone, not merely
   re-routed.
4. Pair sweep: enumerate **every** `(file, IndexInfo)` pair — the 20 top-level
   indexes, every `.mdd` record in each MDX index, every `*_FTS` file, and every
   producer-embedded zip/zim/slob tree — hash inputs before and after, require
   exactly the 3 foreign sound-directory trees rejected and everything else
   accepted, and require no rebuild of the rest.
5. `grep` audit of the worker closure for `.front()` / `[0]` / `memcpy`
   destinations derived from file-controlled sizes; each hit is either fixed or
   has a proven non-empty invariant.
6. End-to-end run above; `coredumpctl` shows no new core.

## Rollout

Packaged build (`packaging/arch/PKGBUILD`, `epoch 2`, `options=('!lto' '!debug')`):
`pkgrel` bump plus `makepkg -si`. The stale sound-directory indexes are then
rebuilt automatically on first start; the immediate manual mitigation (delete
those three files) remains valid and is what unblocks the user today.

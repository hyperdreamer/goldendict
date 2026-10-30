# Btree index foreign-header abort: implementation specification

Approved design: `2026-10-30-btree-index-foreign-header-abort-design.md`, v5, at commit `4fd12d447b096adee93c51df70230b9693b6866c`.
All source locations below refer to that commit, before implementation. “Must” denotes an acceptance requirement. Review dispositions and the compatibility corrections in §10 take precedence over conflicting design prose. This document specifies implementation work; it does not claim that the work or its tests have run.

## 1. Scope, invariants, and non-goals

The immediate failure is an empty-vector `front()` in `BtreeIndex::readNode()` when a record advertises zero compressed bytes (`btreeidx.cc:490`). `SoundDir::IdxHeader` has six 32-bit fields (`sounddir.cc:51`); its current guard checks the signature and version without validating the tree (`sounddir.cc:65`). A foreign seven-field header with the same identity therefore supplies the wrong `IndexInfo`. The implementation must both reject such persisted headers during startup and contain corruption encountered later in descendants, leaf links, chunk blocks, article records, and resource records.

The following invariants apply to all implementations of this specification:

- Validate an offset, count, length, and its arithmetic before allocation, pointer formation, dereference, string scanning, or copying based on it. Validate decompressed node contents before publishing them to traversal.
- Treat invalid persisted metadata as an existing producer's rebuild condition. Apply the same `BtreeIndexing` predicate to every persisted tree, including secondary trees and separate FTS files.
- `openIndex()` must return normally and leave an unusable object on bad input. Distinguish this state from an object on which opening was never attempted.
- A search, article, resource, or FTS request must not call `exit()`/`abort()`, trigger an empty-container assertion, read outside validated storage, or repeat an offset indefinitely because of index bytes. Report failures on the affected request and complete it.
- Preserve all valid writer output: large chains, blocks larger than 65,536 bytes, empty trees, empty resource contents, and bulk scans through more than 4,096 leaves. No fixed `MaxNodeBytes`, maximum resource size, or maximum traversal count may substitute for a format property.
- Preserve persisted structures, field widths/order, compression wrappers, and all format-version values. The existing btree format version is 4 (`btreeidx.hh:35`); the builder's fan-out constants are 64 and 8,192 (`btreeidx.cc:46`).

Non-goals are translating the foreign sound-directory header, changing the application-wide dictionary-loader exception boundary, proving separator sort order or article semantics, recovering a simultaneously rewritten file, and replacing compression codecs. The builder-only compression failure paths are outside the worker repair (`btreeidx.cc:1057`, `btreeidx.cc:1069`). Concurrent replacement/truncation must still fail through checked I/O; this work does not add cross-process file locking. Packaging or release changes are not part of implementation acceptance.

## 2. Data contracts

### 2.1 Persisted bytes and integer domains

| Item | Contract and source of its layout |
|---|---|
| `IndexInfo` | Two `uint32_t` values, `btreeMaxElements`, then `rootOffset`; no new serialized member (`btreeidx.hh:63`). |
| Node record at `o` | Native-format `uint32_t U`, `uint32_t C`, then exactly `C` bytes of the existing zlib stream. Leaf records additionally have a following `uint32_t nextLeaf`, outside the decompressed node (`btreeidx.cc:1061`, `btreeidx.cc:1080`). |
| Leaf payload | `uint32_t e`; repeat `e` times: `uint32_t chainSize`, then that many chain bytes (`btreeidx.cc:926`, `btreeidx.cc:951`). |
| Chain payload | One or more records: word bytes, NUL, prefix bytes, NUL, `uint32_t articleOffset`; exactly consume the payload (`btreeidx.cc:961`). Empty prefix is ordinary. Structural validation must not introduce a non-empty-word semantic rule: two terminators plus an offset require six bytes; ordinary non-empty words require at least seven. |
| Internal payload | `uint32_t 0xffffffff`, `m+1` child offsets, then exactly `m` non-empty NUL-terminated separators (`btreeidx.cc:985`, `btreeidx.cc:1023`). |
| Chunk table | `uint32_t count`, followed by `count` chunk record offsets (`chunkedstorage.cc:104`). Each chunk record has `U`, `C`, and a zlib stream (`chunkedstorage.cc:76`). |
| Chunk address | Upper 16 bits select a table entry; lower 16 bits select the block's starting offset. This limits block starts, not block lengths (`chunkedstorage.cc:31`, `chunkedstorage.hh:17`). |
| ZIP entry | Use the existing local-header fields and variable filename/extra lengths, ZIP little-endian conversion, and raw deflate for method 8 (`zipfile.cc:166`, `indexedzip.cc:69`). Do not interpret ZIP contents as chunk records. |
| MDX/MDD record | Use `sizeof(MdictParser::RecordInfo)` and its existing signed fields: compressed block position, record offset, decompressed block size, compressed block size, record size (`mdictparser.hh:27`). Do not cast a negative field to an unsigned size. |

Use `uint64_t` for file-size comparisons, expansion multiplication, and derived persisted offsets/count products. Check the signed `qint64` returned by file/Qt APIs before conversion. Use subtraction after establishing the left operand's range; do not validate an already-overflowed sum. Check representability in `size_t`, `vector::max_size()`, `qint64`, and the receiving Qt container's size type before narrowing. Read potentially unaligned 32-bit fields with `memcpy`, never a cast-and-dereference. No endian conversion is added to existing native-format index records.

### 2.2 Required declarations and ownership

Add the following declaration next to the existing btree exceptions in `btreeidx.hh:44`:

```cpp
DEF_EX( exIndexInfoUnusable, "The index header does not describe this file", Dictionary::Ex )
```

The public opening API remains, and the shared predicate is a public static member:

```cpp
static bool indexInfoIsSane( IndexInfo const &, File::Class &, Mutex & );
void openIndex( IndexInfo const &, File::Class &, Mutex & );
```

Retain the existing public `findArticles()` logging/empty-result behavior. Add a checked counterpart for request owners that need the error to reach their catch boundary:

```cpp
vector< WordArticleLink > findArticlesChecked( wstring const &,
                                              bool ignoreDiacritics = false );
```

`findArticlesChecked()` contains the actual lookup, antialiasing, and guards and propagates failures. `findArticles()` delegates to it inside its existing catches. Switch article/resource/FTS consumers in §7 to the checked call when an error would otherwise become an indistinguishable empty match. This is necessary because the current method catches errors (`btreeidx.cc:111`), and FTS currently uses that method (`ftshelpers.cc:922`). No global last-error field or thread-local error channel is permitted.

The following protected declarations define the shared traversal state and advance interface. Add `<set>` to the header. Existing callers may omit the final lookup argument; callers that continue across leaves must supply it and retain it.

```cpp
struct TraversalState
{
  std::set< uint32_t > visitedOffsets;
};

static void enterNode( TraversalState &, uint32_t );

char const * findChainOffsetExactOrPrefix( wstring const & target,
                                          bool & exactMatch,
                                          vector< char > & leaf,
                                          uint32_t & nextLeaf,
                                          char const * & leafEnd,
                                          TraversalState * traversal = 0 );

char const * readNextLeaf( uint32_t & nextLeaf,
                           vector< char > & leaf,
                           char const * & leafEnd,
                           TraversalState & traversal );

void readNode( uint32_t offset, vector< char > & out );
vector< WordArticleLink > readChain( char const * & );
```

The helper must be protected, not private: the separate request class accesses inherited protected functions through `BtreeDictionary`, which declares the friendship (`btreeidx.hh:212`). Compile the actual request call, rather than proving accessibility only through a test subclass.

Add private `bool indexUsable = false;`. Initialize both saved pointers to null, both numeric tree members to zero, and `rootNodeLoaded` to false in the constructor; the current constructor only initializes `idxFile` and `rootNodeLoaded` (`btreeidx.cc:52`). The existing private cache and metadata remain private (`btreeidx.hh:146`). The static predicate may initialize a temporary probe's members directly because it is a member of the same class. It must not obtain private access by changing all members to protected.

Reuse the existing `BtreeMinElements`/`BtreeMaxElements` values. They may be moved, unchanged, from `btreeidx.cc:46` to the namespace in `btreeidx.hh` for shared use; do not introduce a second numeric definition. Chunk and ZIP checks must not import those fan-out limits.

## 3. Validation

### 3.1 Node record decision order

`readNode()` is the only node decoder. The caller holds the file mutex. It does not require `indexUsable`, because the opening predicate uses it on its directly initialized probe. Its file pointer and fan-out must already be initialized. It must decode into local storage and publish with `out.swap(decoded)` only after every record and shape check succeeds; failure must not mark the root cache loaded or expose partially decoded bytes.

Perform these steps in order:

1. Obtain the file's non-negative size after flushing any buffered writes through the existing file API. `File::Class::file()` exposes `QFile` and flushes writes (`file.hh:115`, `file.cc:295`). Convert that size to `uint64_t S`; convert `offset` to `uint64_t o`.
2. Require `o <= S` and `8 <= S - o` before seeking/reading the record header. Offset zero is a valid root offset; do not reject it merely for being zero.
3. Read `U` and `C` as `uint32_t`. Require `U != 0`, `C != 0`, and `uint64_t(C) <= S - o - 8` before allocating input storage or reading its payload.
4. Compute `uint64_t limit = uint64_t(C) * 1032u + 64u`. Require `uint64_t(U) <= limit`. The multiplication must be widened before it executes. The factor derives from the deflate maximum match of 258 bytes and a two-bit minimum length/distance pair: `258 * 8 / 2 = 1032`; 64 bytes are boundary slack. This is a deflate property, not an operational size setting.
5. Check API/container representability. Read no more than the already file-bounded compressed payload. Prefer feeding input in bounded pieces; retaining a `C`-byte input vector is permitted only after step 3 and representability checks.
6. Initialize zlib for the existing zlib wrapper. Inflate incrementally with output windows of at most 65,536 bytes. Allocate/grow according to bytes actually produced, never `resize(U)` or `reserve(U)` in advance. The window size is allocation granularity, not an output-size ceiling.
7. Track total input and output in `uint64_t`, independently of platform widths of zlib totals. Reject an output step before it would exceed `limit` or advertised `U`; reject a non-terminal iteration that consumes no input and produces no output when no new input can be supplied. A truncated stream, invalid checksum, zlib error, missing `Z_STREAM_END`, unused bytes within the advertised compressed stream, or final output length unequal to `U` fails validation. Always call `inflateEnd()` after successful initialization, including when a vector growth throws.
8. Perform §3.2 on the actual output. On success, leave the file positioned at `o + 8 + C`, immediately before a leaf's trailer, and publish the validated bytes. The trailer is not part of `U`.

A record/shape violation throws `exFailedToDecompressNode`. Translate short reads and seek errors during the record operation to that exception; retain original diagnostic detail in a warning if useful. Allocation failures may propagate as `std::exception` and are contained by the predicate/request boundary. No process termination is an error mechanism. If the disabled `__BTREE_USE_LZO` branch is retained, document that enabling it requires equivalent codec-specific safety and tests; a deflate bound must not be silently applied to LZO.

### 3.2 Node shape, before any traversal

Let `n = decoded.size()`, with a validated `1 <= m <= BtreeMaxElements`. Use integer cursors until bounds are established. Require `n >= sizeof(uint32_t)` before copying `first`.

For an internal node (`first == 0xffffffff`):

- Compute `headerBytes = 4 + (uint64_t(m) + 1) * 4`; require `headerBytes <= n` before forming the separator pointer or allocating a child array.
- Starting at `headerBytes`, find exactly `m` separators with bounded `memchr` over the remaining region. Each separator has at least one byte before its NUL. A missing terminator, an empty separator/doubled NUL, fewer or extra separators, or bytes after the final terminator is invalid.
- Do not follow child offsets here. Prove each target's record when traversal visits it. Thus a root with a framed but cyclic or corrupt descendant can pass the root predicate and still fail safely during lookup.

For a leaf (`first = e`):

- Require `e <= m`. Set cursor `p = 4`.
- For each entry, require `p <= n` and `4 <= n - p` before copying `chainSize`. Advance by four, require `chainSize != 0` and `uint64_t(chainSize) <= n - p`, and establish `chainEnd = p + chainSize` only then.
- Within `[p, chainEnd)`, repeatedly locate word NUL and prefix NUL with bounded searches, then require four bytes for the article offset. Advance by the full record. At least one complete record must exist and the last record must end exactly at `chainEnd`. Do not apply `strlen()` to unproven bytes or materialize a vector using an unchecked entry count.
- After `e` entries require `p == n`, including when `e == 0`. Consequently the only shape accepted for an empty leaf is exactly four zero bytes.

`readNode()` accepts that empty shape because it has no root/child context. An empty root is a successful empty lookup; an empty descent target or next-leaf target is corruption (§5). Structural validation is required for every child, not only the opening probe's root. This closes the unchecked node-array/string walks at `btreeidx.cc:592`, `btreeidx.cc:611`, and `btreeidx.cc:747`.

### 3.3 Shared predicate, locks, and file position

`BtreeIndex::indexInfoIsSane(info, file, mutex)` must return a Boolean and contain all exceptions from validation. It must never call `openIndex()`, build an index, touch a producer header, or follow all descendants.

Acquire exactly the supplied mutex before the save/seek/read/restore window. Save `file.tell()` as a signed position, obtain the file size, and reject `info.btreeMaxElements == 0`, a value greater than `BtreeMaxElements`, or a root whose eight-byte header cannot fit. A lower value than `BtreeMinElements` is not independently invalid: the predicate's stated accepted range is 1 through `BtreeMaxElements`.

Construct a probe, directly assign its file pointer, mutex pointer, fan-out, and root offset, and invoke `probe.readNode(info.rootOffset, local)` under the same lock. Do not call a traversal entry point whose usability guard would reject the probe. Restore the saved logical file position before releasing the mutex on both success and every validation failure. Position restoration must not throw from a destructor during unwinding. A restoration I/O failure makes the result false; attempt restoration even when decoding failed. Successful calls and ordinary corrupt-input failures must demonstrably preserve position. Physical failure of the restore operation itself is reported as failure, not a fabricated position guarantee.

`Mutex` is recursive (`mutex.hh:14`), so a producer/request already holding that same mutex can invoke the predicate. Acquire no unrelated mutex inside it. Save positions with `File::Class::tell()` rather than a stale raw `QFile::pos()`, because the wrapper accounts for buffered bytes (`file.cc:274`). Serialization applies to the complete save/read/restore sequence, not separately to each read. The predicate performs one root decode per invocation; it does not cache a result across files or opens.

### 3.4 Chunked storage and resource decoding

`ChunkedStorage::Reader` must bound its table offset, four-byte count, and `uint64_t(count) * 4` against the file before resizing the offset vector (`chunkedstorage.cc:117`). An empty table remains legal. `getBlock()` must check the selected chunk index before indexing the table, then validate its record header and compressed region using the same subtraction order as §3.1.

For chunks require `C > 0`, compute the same deflate bound in `uint64_t`, and inflate incrementally to the exact advertised `U`. Do not require `U <= ChunkMaxSize`: the writer can add a whole oversized block before its next flush (`chunkedstorage.cc:47`, `chunkedstorage.hh:17`). A valid empty chunk (`U == 0` with a non-empty, valid empty zlib stream) is legal because the writer explicitly tracks an allocated zero-length block (`chunkedstorage.hh:56`, `chunkedstorage.cc:89`). The `{U=0,C=0}` abort fixture is invalid because no compressed stream exists.

Require `blockOffset <= actualChunkSize`. Preserve the one-past-end result for a legal zero-length block; every consumer of a non-empty record must separately prove its required suffix. For an empty chunk at offset zero, return `chunk.data()` without pointer arithmetic; it may be null and must not be dereferenced. Table/address failures use `exAddressOutOfRange`; invalid chunk record/stream/length uses `exFailedToDecompressChunk`. Convert file errors while decoding a chunk to the latter, and clear/unpublish partial output. The Reader continues to use its caller's existing file lock; it must not invent an unsynchronized private mutex around a shared file position.

For `IndexedZip::loadFile()` (`indexedzip.cc:49`), check the local header and every variable-length region against the ZIP stream before deriving the payload position. After reading the local header, require `payloadPos <= zipSize` and `compressedSize <= zipSize - payloadPos`, with non-negative signed sizes converted only after checks. Method 0 requires `compressedSize == uncompressedSize`; return success with an empty output for a valid empty entry and skip its read/copy. Method 8 uses raw `inflateInit2(..., -MAX_WBITS)`, the wide expansion bound, incremental output, exact advertised output length, and a complete compressed stream. Never use `ChunkMaxSize` as a ZIP limit. Unsupported methods and malformed records return false with cleared output; request owners must turn a known malformed referenced resource into their request error. A normal missing name remains a normal miss. Do not narrow `header.compressedSize` to `int` for comparison as at `indexedzip.cc:85`.

For MDX and MDD, prove a complete `RecordInfo` suffix before `memcpy`; reject negative positions/sizes before conversion; prove the mapped compressed source region against its source file; and check codec/API size representability before mapping/decompression. Preserve existing codec dispatch and checksum checks (`mdictparser.cc:242`). After decompression, let `D` be actual output length: require `recordOffset <= D` and `recordSize <= D - recordOffset`; only then resize the result and copy. A zero `recordSize` succeeds without `front()` or a copy. A claimed decompressed size must agree with the decoder's actual result. Do not apply a deflate bound to an LZO or plain MDX block. These requirements apply to `IndexedMdd::loadFile()` and the article loader (`mdx.cc:152`, `mdx.cc:975`).
For the stylesheet table (`mdx.cc:420–437`) prove the four-byte key and each `uint32_t` length against the index file before `resize`; skip `front()`/`read` entirely when a length is zero; and convert with an explicit byte count plus a checked terminator instead of `QString::fromUtf8(buf.data())`, which requires a NUL byte that a `vector<char>` of exactly that size does not have. For the chunk-backed description (`mdx.cc:928–943`) use `idxHeader.descriptionSize` as the length and never build a `std::string` from an unterminated chunk pointer.

## 4. `openIndex` state machine

The state is local to a `BtreeIndex` object; usability is not persisted. A file/mutex reference must outlive all operations on that object. Reopening and traversal on the same object require the caller's serialization; no new guarantee is made for concurrent opens with different mutexes.

| State | Saved pointers | Fan-out/root/cache | Observable traversal result |
|---|---|---|---|
| Never opened | Both null | Zero/zero, `rootNodeLoaded=false`, empty `rootNode`, `indexUsable=false` | The guard throws `exIndexWasNotOpened`. A catching public wrapper follows its documented catch behavior. |
| Open attempted, unusable | Point to the supplied file/mutex | Zero/zero, unloaded/empty cache, `indexUsable=false` | The second guard throws `exIndexInfoUnusable`. No node is read and no old cache is reused. |
| Usable | Point to the supplied file/mutex | Validated fan-out/root, `indexUsable=true`; cache initially unloaded/empty | Normal checked traversal; descendant corruption still throws. |

On every call, including usable-to-usable reopen:

1. Reset `indexNodeSize=0`, `rootOffset=0`, `rootNodeLoaded=false`, `rootNode.clear()`, and `indexUsable=false` before validation.
2. Assign the supplied file/mutex pointers even when the proposed header is bad.
3. Call the shared predicate. Catch any escaping exception defensively and treat it as false.
4. If false, return normally in the unusable state. If true, commit only the proposed fan-out/root and finally set `indexUsable=true`. Do not retain the probe's decompressed node as this object's cache without a separately specified synchronization change; the implementation here leaves it unloaded.

All transitions from never-opened, unusable, or usable follow this sequence. In particular `valid → invalid → valid` must never retain the first valid tree's cache. A later traversal failure does not silently reopen, rebuild from a worker, or mark an old root valid. Rebuild routing belongs to §6.

## 5. Traversal guard table

Each entry's first operations must be `if (!idxFile) throw exIndexWasNotOpened();` followed immediately by `if (!indexUsable) throw exIndexInfoUnusable();`, before locking a saved mutex, reading private offsets, using a pointer argument, or folding the query. A small private guard may implement this once, but each row must invoke it before its own work. Wrapper guards may delegate only when delegation occurs before any state access; for explicit inventory compliance, keep a guard in each of the seven existing entries below.

| Entry and current source | Guard placement and failure visibility | State/loop obligations |
|---|---|---|
| `findArticles`, `btreeidx.cc:83` | Inside its try/delegated checked lookup; catches, logs `Articles searching failed`, returns an empty vector, including on invalid/default state. | New checked implementation owns the lookup; do not return a partly built result on failure. |
| `findAllArticleLinks`, `btreeidx.cc:1271` | Before creating/delegating its traversal; errors propagate to its request caller. | Delegates to the guarded bulk implementation. |
| `getAllHeadwords`, `btreeidx.cc:1263` | Before delegation; errors propagate. | Delegates to the guarded bulk implementation. |
| `findArticleLinks`, `btreeidx.cc:1281` | Before `currentNodeOffset = rootOffset` or saved-mutex dereference. | New local traversal state, root entry, all left descents, and every subsequent leaf share it. |
| `getHeadwordsFromOffsets`, `btreeidx.cc:1406` | Before root/mutex access. | Same state across initial descent and every leaf; retain existing cancellation/early-completion behavior. |
| `findChainOffsetExactOrPrefix`, `btreeidx.cc:514` | Before locking or loading the root. | Use supplied state or a local one, enter the root even if cached, enter every child, and pass the same state into its own next-leaf advance. |
| `readChain`, `btreeidx.cc:855` | Before its first four-byte read or changing the caller's pointer. | Its pointer must originate in a validated chain from a currently live node buffer. Whole-node inner framing has already been proven. |
| New `findArticlesChecked` | Before query work; propagates. | Same result semantics as successful `findArticles`, with one local traversal state. |
| New `readNextLeaf` | Before using arguments or the saved file. | Non-empty-leaf proof and repeated-offset check described below. |

`readNode()` deliberately has no usability guard (§3.1); it is the validator's low-level decoder. A test probe opens a sane bootstrap file first and then passes malformed non-root offsets to this decoder; it does not need a production bypass for `indexUsable`.

`enterNode(state, offset)` must insert the offset into `visitedOffsets` before reading/interpreting that logical node; a failed insertion throws `exCorruptedChainData`. There is no `MaxTraversalNodes=4096` and no other arbitrary count ceiling. A root cache hit still counts as entering the root in this traversal. Do not charge an offset twice merely because separate validation and consumption steps inspect the same already-loaded node; charge transitions to a logical node, including from a link back to an already visited node. Node framing makes each unique-node inner walk finite, and a repeated-offset check bounds cycles without rejecting long acyclic scans.

A prefix/stem request must retain one state across its initial lookup and the leaf scan for that lookup target. A later suffix-chopping search starts a new state when it restarts from the root; otherwise valid repeated searches within one request would be mistaken for cycles (`btreeidx.cc:415`). Bulk methods keep their state until the complete scan ends. State is never a member shared across requests, and opening validation does not consume a request's state.

The protected `readNextLeaf()` performs the following transaction under the same file lock:

1. If `nextLeaf == 0`, return null without changing buffers; callers treat it as end of scan. Otherwise copy the target offset locally and enter it in the traversal state.
2. Decode the target into a local vector with `readNode()`. Copy its first word only after that decoder's minimum-size proof.
3. Require `first != 0xffffffff` and `1 <= first <= indexNodeSize`. An internal node or four-byte empty leaf throws `exCorruptedChainData` before returning a chain pointer, even though either can be a structurally valid node elsewhere.
4. At the file position immediately after that target's compressed payload, read its four-byte following-link trailer with a file-range check. A missing trailer is a contained `File::exReadError`. Do not read a trailer for an internal node during ordinary descent: only leaves have that field.
5. Commit the vector, `leafEnd`, and following `nextLeaf` together. Return `leaf.data() + sizeof(uint32_t)` only after the non-empty proof. Do not leave a caller's `leafEnd` pointing into replaced storage after failure.

| Next-leaf advance that must call the helper | Existing failure to remove | Required test route |
|---|---|---|
| `BtreeWordSearchRequest::findMatches`, `btreeidx.cc:386` | Internal-node `exit(1)` at `btreeidx.cc:407`. | Start at a real matching chain and make this scan advance. |
| `findChainOffsetExactOrPrefix`, `btreeidx.cc:826` | Returns a chain pointer into an unchecked internal/empty target (`btreeidx.cc:845`). | Gap query `word00001z`; fail before `readChain`. |
| `findArticleLinks`, `btreeidx.cc:1382` | Duplicated type check without shared empty-target proof. | Bulk scan with bad first trailer. |
| `getHeadwordsFromOffsets`, `btreeidx.cc:1499` | Duplicated type check without shared empty-target proof. | Offset request requiring a later leaf. |

The three descent loops at `btreeidx.cc:552`, `btreeidx.cc:1307`, and `btreeidx.cc:1433` must all use `enterNode()` before following a child. After following a child require a non-empty leaf when it is a leaf; an empty root returns no matches. In `findMatches`, check `chain.empty()` before `chain[0]` (`btreeidx.cc:330`); skip processing that chain, then perform normal pointer/end/advance progress. Do not `continue` without advancing. The decoder rejects empty chains, but this call-site defense must still exist.

## 6. Rebuild wiring table

Each old-or-bad guard keeps its existing signature/version/parser/folding/optional-ZIP checks and adds `!BtreeIndex::indexInfoIsSane(pair, indexFileObject, mutex)` with OR semantics. Read its header completely before inspecting fields; an opening/short-read/parse exception returns “old or bad” instead of escaping before the rebuild branch. A local guard-owned file can use a local mutex; a file shared with a live dictionary must use that dictionary's existing mutex for all its operations. Never validate one file's pair against a similarly named source/ZIP/MDD file.

All 16 producer files are required, including the shared FTS producer. In the following table, `main pair` means `indexBtreeMaxElements/indexRootOffset` in that producer's header; `<id>` is the existing dictionary ID. The listed branch is the existing rebuild decision that must consume the new predicate result.

| Producer | File/pair evidence | Guard to extend | Rebuild branch |
|---|---|---|---|
| SoundDir | `<indexdir>/<id>`, main pair (`sounddir.cc:51`, open at `sounddir.cc:132`) | `indexIsOldOrBad`, `sounddir.cc:65` | `sounddir.cc:444` |
| Stardict | Main pair (`stardict.cc:136`, `stardict.cc:290`) | `indexIsOldOrBad`, `stardict.cc:159` | `stardict.cc:1974` |
| DSL | Main pair (`dsl.cc:94`, `dsl.cc:470`) | `indexIsOldOrBad(file, hasZipFile)`, `dsl.cc:138` | `dsl.cc:2176` |
| GLS | Main pair (`gls.cc:394`, `gls.cc:560`) | `indexIsOldOrBad(file, hasZipFile)`, `gls.cc:420` | `gls.cc:1515` |
| XDXF | Main pair (`xdxf.cc:100`, `xdxf.cc:309`) | `indexIsOldOrBad`, `xdxf.cc:130` | `xdxf.cc:1203` |
| SDict | Main pair (`sdict.cc:95`, `sdict.cc:220`) | `indexIsOldOrBad`, `sdict.cc:115` | `sdict.cc:768` |
| Aard | Main pair (`aard.cc:105`, `aard.cc:323`) | `indexIsOldOrBad`, `aard.cc:124` | `aard.cc:866` |
| BGL | Main pair (`bgl.cc:61`, `bgl.cc:300`) | `indexIsOldOrBad`, `bgl.cc:91` | `bgl.cc:1200` |
| Dictd | Main pair (`dictdfiles.cc:78`, `dictdfiles.cc:198`) | `indexIsOldOrBad`, `dictdfiles.cc:94` | `dictdfiles.cc:600` |
| LSA | Main pair (`lsa.cc:62`, `lsa.cc:219`) | `indexIsOldOrBad`, `lsa.cc:76` | `lsa.cc:569` |
| ZipSounds | One persisted main pair, consumed twice (`zipsounds.cc:59`, `zipsounds.cc:162`, `zipsounds.cc:170`) | `indexIsOldOrBad`, `zipsounds.cc:73` | `zipsounds.cc:441` |
| ZIM | Main pair (`zim.cc:135`, `zim.cc:766`) | `indexIsOldOrBad`, `zim.cc:516` | `zim.cc:1663` |
| SLOB | Main pair (`slob.cc:73`, `slob.cc:720`) | `indexIsOldOrBad`, `slob.cc:141` | `slob.cc:1744` |
| EPWING | Main pair (`epwing.cc:51`, `epwing.cc:222`) | `indexIsOldOrBad`, `epwing.cc:71` | `epwing.cc:1027` |
| MDX | Main pair (`mdx.cc:70`, `mdx.cc:441`) | `indexIsOldOrBad(dictFiles, file)`, `mdx.cc:1377` | `mdx.cc:1435` |
| FTS | Separate `<indexdir>/<id>_FTS`, `FtsIdxHeader` main pair (`ftshelpers.hh:31`, `ftshelpers.cc:1301`) | `ftsIndexIsOldOrBad(file, dict)`, `ftshelpers.cc:26` | Existing per-dictionary `makeFTSIndex` guards; e.g. `mdx.cc:489`, `zim.cc:1023`, `slob.cc:1133` |

The first 15 rows use their `<indexdir>/<id>` file. FTS must preserve its version expression `CurrentFtsFormatVersion + dict->getFtsIndexVersion()` (`ftshelpers.cc:38`) and validate against the FTS file, not the dictionary's main index. Use `dict->getFtsMutex()` when that live object is available.

| Owner/secondary tree | Persisted pair and current consumer | Required old-or-bad wiring and absence rule |
|---|---|---|
| MDX, every `.mdd` volume | Each name-plus-pair record in the main index's `mddIndexInfos` table (`mdx.cc:445`, `mdx.cc:1590`); `IndexedMdd::openIndex` consumes it at `mdx.cc:468`. | Parse all records and validate each pair against the main index file, never the `.mdd` source file. Any bad record/pair rebuilds the owning main index. Zero volumes is legal when the existing source count is zero. |
| Stardict ZIP | `zipIndexBtreeMaxElements/zipIndexRootOffset` (`stardict.cc:150`), open at `stardict.cc:300`. | Add to the owning guard. `{0,0}` means absent; either nonzero means a present pair and must pass the shared predicate. Keep the existing source-ZIP availability check. |
| DSL ZIP | Same two fields (`dsl.cc:114`), open at `dsl.cc:480`. | Same rule; preserve `hasZipFile` and `zipSupportVersion` behavior (`dsl.cc:148`). |
| GLS ZIP | Same two fields (`gls.cc:411`), open at `gls.cc:570`. | Same rule; preserve its existing ZIP-version condition (`gls.cc:430`). |
| XDXF ZIP | Same two fields (`xdxf.cc:120`), open at `xdxf.cc:295`. | Same present/absent rule; validate any persisted nonzero pair. |
| ZIM resources | `resourceIndexBtreeMaxElements/resourceIndexRootOffset` (`zim.cc:141`), open at `zim.cc:770`. | Required pair in the owning header; validate even for a resource tree with zero entries, represented by a sane empty root. Do not borrow the optional ZIP `{0,0}` convention. |
| SLOB resources | Same resource fields (`slob.cc:79`), open at `slob.cc:724`. | Same required-pair rule. |
| ZipSounds second consumer | `zipsFile.openIndex` uses the identical main pair and file (`zipsounds.cc:170`). | No invented second pair; validate the persisted pair once in the producer guard and retain inert-open protection at both consumers. |
| `IndexedZip` | Generic inherited `openIndex`, called by the owning formats (`indexedzip.hh:14`). | Inherits state safety; owning producer rows provide rebuild routing. No independent persisted header to invent. |

The MDX table parser must follow the actual writer: `uint32_t filenameByteLength`, exactly that many filename bytes, then two `uint32_t` values (`mdx.cc:1590`). Prove `dictFiles.size() >= 1` before subtracting one; require count agreement using a wide comparison. Prove `tableOffset <= S` and, before iteration, that `count <= (S - tableOffset) / 12`, the minimum bytes for length plus pair per record. Before every name skip prove its length fits while reserving eight bytes for that record's pair. Read a filename with its explicit length, not a presumed NUL terminator. The table has no persisted terminal offset: require every claimed record to fit, without demanding that the table end equal the file end. The predicate restores the parser's current position after each root probe, so the next table record remains readable.

Retain the same checks in deferred MDX initialization before indexing the parsed volume vector, even after a startup guard has accepted the file (`mdx.cc:446`, `mdx.cc:468`). A file changed after the guard must produce a contained deferred-init/request error. Other secondary opens likewise retain §4 protection after a guard accepted their header. Do not route rebuild solely by waiting for an inherited `openIndex()` to become inert; that contains failures but leaves the dictionary permanently broken.

## 7. Worker-safety sweep

### 7.1 Error propagation and bounded operations

Existing request error strings are mutex protected, and `finish()` emits completion once (`dictionary.cc:45`, `dictionary.cc:55`). Use that existing channel. At the outer boundary of each affected asynchronous `run()`, catch `std::exception`, set a non-empty per-dictionary error from `what()`, clear success-only partial data/matches under `dataMutex`, and finish. Add a catch-all with a stable non-empty fallback message. Preserve cancellation and runnable exit-semaphore release; ensure no early return skips them. A synchronous article/resource API must return `DataRequestInstant(errorString)` on corruption rather than throw past its request boundary (`dictionary.hh:225`). A normal absence/cancellation keeps its existing semantics.

The prefix catches currently only log (`btreeidx.cc:424`), and the FTS catch currently only logs (`ftshelpers.cc:1332`). Both must additionally set the request error. `findMatches()` can return to `run()` for its normal final `finish()`; do not require a second observable completion signal. FTS and dictionary retrieval must use `findArticlesChecked()` wherever the existing catching lookup would hide corruption. Inert `openIndex()` is not a successful FTS/resource open: a checked traversal must expose it at the request boundary, and FTS may not label a corrupt tree an ordinary empty result or silently switch to a full scan.

For all chunk consumers, first reject an empty vector when a non-empty record is required. Establish the returned pointer's integer offset within its owning buffer before subtracting/adding; a required fixed prefix of `k` bytes needs `k <= bufferSize - offset`. Bounds established for a whole chunk are not proof of a record's fixed header or length-prefixed fields. Strings must have a bounded NUL or an explicitly checked byte count. Every zero-length read/copy/conversion skips `front()` and pointer arithmetic on an empty vector; checking the byte count after taking `&v.front()` is too late. No `memcpy(..., ..., 0)` exception to this rule exists.

### 7.2 Concrete worker closure

The following is the required initial inventory, not permission to leave additional reachable hits unchecked. Locations are current unsafe reads/copies or their request boundaries. Fix the actual production path or record the existing dominating proof during the final source sweep.

| Owner/current sites | Required proof or repair and reporting boundary |
|---|---|
| Btree prefix/stem: `btreeidx.cc:316`, `btreeidx.cc:330`, `btreeidx.cc:386`, `btreeidx.cc:424` | Guard state, validated inner chains, empty-chain defense, shared advance, and request error catches. No `exit(1)` remains in the worker closure. |
| Chunk reader: `chunkedstorage.cc:117`, `chunkedstorage.cc:135`, `chunkedstorage.cc:146` | Table/frame bounds, actual-size incremental inflation, zero-stream handling, and checked block-start suffix (§3.4). |
| MDX/MDD resources: `mdx.cc:152`, `mdx.cc:180`, `mdx.cc:791` | Complete `RecordInfo`, signed checks, source-map range, decompressed slice proof, zero-size copy skip; `MddResourceRequest::run` catches all failures and reports them. |
| MDX articles: `mdx.cc:613`, `mdx.cc:975` | Same `RecordInfo`/source/decompressed-region proofs in `loadArticle`; do not rely on the MDD-only fix. Errors reach the article request rather than a success-only HTML copy. |
| MDX cached resource redirection: `mdx.cc:1170`, `mdx.cc:1219` | Guard empty resource-name indexing; a repeated `@@@LINK=` target must terminate with an error. The resource worker's corresponding set/loop is at `mdx.cc:807`, `mdx.cc:821`; do not `continue` with the same repeated target forever. Validate the UTF-16LE redirect prefix/length before decoding. |
| Indexed ZIP: `indexedzip.cc:49`, `indexedzip.cc:74`, `indexedzip.cc:88` | ZIP file bounds, valid empty entry, method-specific sizes, incremental raw deflate; clear output on false. Apply at every owning resource caller, not only ZipSounds. |
| SoundDir: `sounddir.cc:211`, `sounddir.cc:255`, `sounddir.cc:335`, `sounddir.cc:372` | Checked lookup and chunk decode; bounded filename terminator; no `chunk.back/front` on empty output; empty file reads skip `front()`. Enclose synchronous article/resource work in an error-request boundary rather than swallowing corrupt-address exceptions as normal misses. |
| ZipSounds: `zipsounds.cc:255`, `zipsounds.cc:301`, `zipsounds.cc:372` | For a record prove `uint16_t nameLength`, then `nameLength` bytes, then a `uint32_t ZIP offset` before each copy/use (`zipsounds.cc:375`, `zipsounds.cc:381`). Existing pointer-less-than-end checks do not prove these widths. Return error requests for malformed records. |
| Stardict: `stardict.cc:368`, `stardict.cc:370`, `stardict.cc:911`, `stardict.cc:971` | Prove two 32-bit article properties plus any following headword terminator; bound each binary article field size before advancing. Guard `loadString(0)` (`stardict.cc:355`). Keep article/headword catches and make resource catches set the request error (`stardict.cc:1432`, `stardict.cc:1737`). |
| DSL: `dsl.cc:440`, `dsl.cc:567`, `dsl.cc:1396` | Prove two `uint32_t` article properties; for abbreviation tables prove count and every key/value length before copy. Both article and `getArticleText` paths require it. Enclose resource work and avoid zero-length image/data copies (`dsl.cc:1938`, `dsl.cc:2012`, `dsl.cc:2035`). |
| GLS: `gls.cc:699`, `gls.cc:704`, `gls.cc:1031` | Prove two article properties; guard headword vectors before `front()` even when a lookup is empty. Preserve zero-length article/result handling (`gls.cc:1219`); set resource errors (`gls.cc:1305`, `gls.cc:1443`). |
| XDXF: `xdxf.cc:264`, `xdxf.cc:661`, `xdxf.cc:664` | Prove the nine-byte type/offset/size prefix without evaluating `chunk.front()` first; bound abbreviation tables and sized description/name reads (`xdxf.cc:245`, `xdxf.cc:378`). Set article/resource errors at their request boundary (`xdxf.cc:518`, `xdxf.cc:1046`). |
| BGL article/headword/metadata: `bgl.cc:378`, `bgl.cc:400`, `bgl.cc:561` | Bound the headword, displayed headword, article, and description strings separately; prove icon/metadata lengths within the returned chunk (`bgl.cc:335`). Headword workers must contain storage exceptions as well as UTF-8 failures (`bgl.cc:591`). |
| BGL resources: `bgl.cc:1011`, `bgl.cc:1034`, `bgl.cc:1050` | Before allocating, bound each resource-list filename and its following offset against the index file; validate the target's `U/C` frame, use bounded incremental zlib, and support valid empty resource output. Add the missing outer request catches; do not leave this parallel decompressor unchanged after fixing `ChunkedStorage`. |
| Aard: `aard.cc:447`, `aard.cc:557`, `aard.cc:716` | Bound source article/resource length against its file and check representability before resize; skip empty-vector reads and result copies (`aard.cc:821`). Catch errors over the whole request, including initial checked lookups outside inner article catches. |
| SDict: `sdict.cc:389`, `sdict.cc:545`, `sdict.cc:700`, `sdict.cc:716` | Same file-range/zero-copy rules for article/resource reads and source decompression lengths; request catches must cover initial lookup and decoding, not just inner rendering. |
| Dictd: `dictdfiles.cc:417`, `dictdfiles.cc:421` | Checked lookup, source-read length checks, and empty result copy skip. Preserve the instant-request catch boundary and set its error instead of returning a failed success request. |
| LSA: `lsa.cc:327`, `lsa.cc:425`, `lsa.cc:458` | Checked lookup and validate source fields before computing WAV allocation. Widen/sample-count multiplications and require the output to contain `sizeof(WavHeader)` before its cast/write. An empty sound may still have a non-empty WAV header; distinguish that from an empty result vector. Report errors on the synchronous resource request. |
| EPWING: `epwing.cc:299`, `epwing.cc:410`, `epwing.cc:497`, `epwing.cc:732` | Prove complete page/offset fields using their declared widths before copies, in both article-text paths; keep decoder errors inside the request boundary and skip empty output copies (`epwing.cc:651`, `epwing.cc:774`). Feature-conditional builds must compile these paths too. |
| ZIM: `zim.cc:438`, `zim.cc:625`, `zim.cc:639`, `zim.cc:646` | Before the first cluster-offset read require its 4/8-byte width. Prove the first offset, offset-table divisibility/count, selected pair, ordered blob range, and range within actual decompressed cluster using wide arithmetic. Bound redirect cycles, not only a direct self-redirect (`zim.cc:593`). Guard empty result/resource copies and set request errors (`zim.cc:1341`, `zim.cc:1544`, `zim.cc:1606`). |
| SLOB: `slob.cc:449`, `slob.cc:583`, `slob.cc:592` | Prove offset-table size, bin index, each four-byte field, and bin data length before subtraction/multiply/substring. Do not subtract four from a shorter unsigned buffer length. Enclose initial lookup and article/resource decoding, and skip empty copies (`slob.cc:1441`, `slob.cc:1629`, `slob.cc:1691`). |
| MDX metadata: `mdx.cc:420`, `mdx.cc:430`, `mdx.cc:434`, `mdx.cc:938` | Prove the stylesheet key, count and both lengths against the index file, skip zero-length reads/copies, and convert with an explicit byte count; bound the descriptor by `descriptionSize`; surface failures through the deferred-init error and the owning request instead of aborting or scanning past the chunk. |
| FTS: `ftshelpers.cc:933`, `ftshelpers.cc:1260`, `ftshelpers.cc:1332` | In every posting-list reader prove the four-byte count and `count <= remaining/4`; copy unaligned offsets safely. Use checked btree lookup; detect an inert FTS tree; errors set the FTS request string and finish without success data. Apply to both whole-word and combined-index paths. |

Size-derived UI/image/transcoding work downstream of a record is included when that record can yield empty data. A newly safe decoder does not prove every caller can call `front()` on a valid empty result. Preserve existing missing-file behavior separately from malformed referenced storage.

Run a final `rg` inventory over all 16 producers, `btreeidx.*`, `chunkedstorage.*`, `indexedzip.*`, `mdictparser.*`, and directly called readers. Search `.front()`, `.back()`, `[0]`, `strlen`, `memcpy`, `resize`, `exit`, `abort`, and exception boundaries. Classify each reachable hit with its bounds proof and test, or its builder-only/unreachable reason with a source citation. The observed builder failures at `btreeidx.cc:1057`/`btreeidx.cc:1069` are not a reason to ignore a worker hit. The sweep is acceptance evidence, not a new request for operator approval.

Record the sweep as `reports/worker-sweep.md` in the PM run root with one row per hit and the columns `file:line`, `operation` (`front`/`back`/`[0]`/`strlen`/`memcpy`/`resize`/`exit`/`abort`/`catch`), `owning request`, `reachable from worker` (yes/no + citation), `bounds proof or repair`, `named test`, `feature configuration`, and `disposition`. Every `reachable = yes` row must name either a test or a repair; every `no` row must cite the dominating proof or the builder-only caller. The inventory is not complete while any row is blank.

## 8. Test specification

### 8.1 Suite and fixture architecture

Add `tests/tst_btree_index/tst_btree_index.pro` and `tst_btree_index.cc`, `TARGET = tst_btree_index`, using `../testsupport.pri`. Update the aggregate's closed inventory and count from 18 to 19, rather than relying on discovery alone (`tests/tests.pro:9`, `tests/tests.pro:114`). This is a `QCoreApplication` suite: no Chromium initialization, GUI display, external dictionary corpus, or network. Follow the environment gate and watchdog APIs (`tests/support/fixtures.hh:69`, `tests/support/fixtures.hh:75`). A 60-second suite watchdog and per-subprocess cycle limits are required; the ordinary aggregate keeps its existing 900-second outer timeout.

Link the real `btreeidx.cc`, `chunkedstorage.cc`, `file.cc`, `dictionary.cc`, `mutex.cc`, `utf8.cc`, `wstring.cc`, `wstring_qt.cc`, `folding.cc`, `wildcard.cc`, `fsencoding.cc`, `gddebug.cc`, `indexedzip.cc`, and required production dependencies. Include the real owner/parser closures needed by the producer and worker rows below; feature conditions and libraries must follow `tests/testsupport.pri` and the application build. MOC `dictionary.hh`/other real `Q_OBJECT` headers as required. Do not duplicate a decoder or producer guard in test code. Per-function sections/linker GC, as used in `tests/tst_regex/tst_regex.pro:81`, may keep unreferenced application components out of this GUI-less binary.

For file-local producer guards or private record readers, use narrow test-only forwarding adapters compiled in their real translation units under a test define, or extract the actual small production reader so both caller and test use it. Such adapters may expose existing private state for observation and inject fixture data, but may not implement a parallel validator. Tests must call each actual guard/consumer in §6/§7 and assert behavior. Merely scanning for the predicate's name, running the shared predicate on a copied header, or linking an uncalled consumer does not prove producer wiring. Feature-conditional EPWING/codec rows require the corresponding supported build configuration; do not silently count an uncompiled row as passed.

Use `QTemporaryDir` and production `File::Class`/builders. Define `W32(x)` as the existing index's native-endian four-byte serialization, using `memcpy` into fixture bytes; ZIP fixtures use explicit little-endian fields. A test-only subclass exposes `readNode`, `readChain`, `findChainOffsetExactOrPrefix`, `readNextLeaf`, and traversal state. A minimal concrete `BtreeDictionary` fixture implements unrelated abstract metadata accessors and uses the real prefix request. Do not stub `findMatches`, the advance helper, node reads, or request error handling. Observe public failures through return values, captured warnings, error strings, and completion signals; observe throwing low-level contracts through the subclass.

All malformed-node tests start with a sane, open bootstrap tree and then read a separate appended malformed record, or mutate a descendant after opening a sane root. Thus an opening rejection cannot make an alleged decoder test pass without reaching its decoder.

### 8.2 Exact fixture recipes

- **Single link:** `link(w,p,a) = bytes(w) + 00 + bytes(p) + 00 + W32(a)`. Normal leaf `L = W32(1) + W32(link.size()) + link`, using `w="a"`, empty prefix, and article offset 7 unless stated otherwise.
- **Record:** `R(P) = W32(P.size()) + W32(z.size()) + z`, where `z = compress2(P, Z_BEST_COMPRESSION)` using the real linked zlib. Append `W32(next)` for a leaf. Mutate fields in a copy; use this framing helper, not a copy of the production shape checker.
- **Empty leaf:** `R(W32(0)) + W32(0)`. A next-link target with this payload is valid at the decoder but must fail at the advance helper.
- **Internal node for m:** `W32(0xffffffff)`, `m+1` offsets, then the exact non-empty strings `s00000\0` through `s<m-1>\0`. Offsets may be assigned during fixture assembly, then the payload recompressed. Internal-node fixtures need not be semantically sorted when a test exercises structural/cycle behavior only.
- **Foreign header:** the seven native 32-bit words are `{0x58524453, 11, 142000, 28, 2057158, 378, 2053531}`. The first six are the old reader's exact view: signature, version, sound count, chunk-table offset, impossible fan-out, root offset. The six-field view is 24 bytes and the foreign header is 28 bytes. Feed the actual SoundDir guard a temp file beginning with these bytes; separately require `indexInfoIsSane(IndexInfo(2057158,378), ...) == false`. Do not treat file length 28 versus 24 alone as an index-header-size discriminator, because normal indexes contain data after their header (`sounddir.cc:467`).
- **130-word builder tree:** call production `addSingleWord` for `word00000` through `word00129`, article offsets 0 through 129, then `buildIndex`. Require resulting `m == BtreeMinElements`. Locate the first leaf via the root's first child and decode its payload in fixture setup; verify its last chain is `word00001`, and the next separator is `word00002` before using the pinned gap query `word00001z`.
- **Internal next target:** rewrite that first leaf's trailer to the builder's internal root and append at least four readable bytes after the root's compressed payload. Builder children precede the final root (`btreeidx.cc:1015`, `btreeidx.cc:1061`); without this padding, the old code's trailer short-read can mask the missing type check. The matching-scan route starts at `word00000` with prefix `word`; the gap route uses `word00001z`. The two bulk routes request data known to require the next leaf.
- **Empty next target:** append the exact empty-leaf record, repoint the same trailer, and run all four advance routes. Each must reject at the helper before any `readChain` call for that target. Use a narrow test-only counter at `readChain` entry if needed; sanitizer cleanliness alone is not proof of the rejection point.
- **Large builder chain:** use one indexed key `word00000` with 1,500,000 distinct article offsets in its chain, then production `buildIndex`. Each record is 9 word bytes + NUL + empty-prefix NUL + 4 offset bytes = 15 bytes, so this recipe produces a leaf above 16 MiB. Alternatively use the reviewed 7-byte word `"word000"`, which gives the exact 19,500,008-byte leaf (`4 + 4 + 1,500,000 * 13`). Assert the actual expected size for the chosen recipe and return every offset; do not replace the builder with a hand-written oversized record.
- **High deflate ratio:** create a structurally valid one-entry leaf whose word has `32*1024*1024 - 14` ASCII `a` bytes, empty prefix, and a zero article offset. Its payload is exactly 32 MiB. Compress it with zlib, assert `U > uint64_t(C)*516+64` and `U <= uint64_t(C)*1032+64`, and require successful node decode. This tests the proven factor without depending on a fixed compressed size or a `/tmp` reviewer artifact.
- **Long scan:** assemble 4,100 individually framed, non-empty, linked one-entry leaves with unique ascending words and offsets; place a sane internal root with `m=1` above the start. It is sufficient to prove an acyclic structurally valid scan, not builder balancing. Require actual bulk results from every leaf. The result must exceed 4,096 distinct leaves; a test containing thousands of chains in a single leaf does not qualify.
- **Oversized chunk:** call production `ChunkedStorage::Writer::startNewBlock`, append 65,537 known nonzero pattern bytes in that one block, finish, reopen, and compare all bytes through `Reader`. This uses writer behavior, not a fabricated oversized advertised size. For an empty chunk, use `U=0` with `compress2` of a zero-length input and a real non-empty zlib stream; `{0,0}` is a separate malformed fixture.
- **Sparse oversized file:** to exercise a forged `C` far beyond the real data without writing gigabytes, create the fixture with `QTemporaryFile`, `resize()` it to the intended apparent size (sparse), then write only the bytes the fixture needs. Assert rejection happens before any output-sized allocation by checking that the process resident size does not grow by the advertised amount; an `ftruncate`-backed file is acceptable and must be documented in the test comment.
- **Byte-boundary records:** for every fixed/sized record consumer, place its required prefix at the final 0 through `k-1` bytes of a valid chunk and test each short suffix. Include the exact-size success case, an oversized following length, missing terminator, and zero-length data where its format permits it. Recompress the outer chunk, so failure reaches the record reader after successful chunk decoding.

### 8.3 Required named tests

There are **50 named tests**, T01–T50. Each row is one required QTest slot; `_data` providers and parameter rows do not increase the count. Names are normative. Environment/watchdog harness self-checks are not additional feature tests.

| ID / required slot | Fixture, operation, and exact acceptance |
|---|---|
| T01 `builds_and_reads_back` | Production-built empty, one-word, alias/prefix, and multi-leaf trees: predicate true, open succeeds, exact/case-folded lookups and article offsets unchanged. Empty tree returns an empty successful result. |
| T02 `open_index_terminates` | Run a sane open and subsequent checked query in a child process with a 5-second wall limit. It exits normally and returns the expected link; no recursive predicate/open cycle or deadlock. |
| T03 `foreign_sounddir_header_is_not_sane` | Exact seven-word fixture: shared predicate false for its six-word view; actual SoundDir old-or-bad guard true with unchanged identity fields. Valid local SoundDir header/control remains accepted. |
| T04 `insane_index_info_is_inert` | Open bad info: no exception; public `findArticles` returns empty and emits its failure warning; exposed protected lookup and `findArticlesChecked` throw `exIndexInfoUnusable` before I/O. |
| T05 `zeroed_node_record_throws` | Appended `W32(0)+W32(0)` record: `readNode` throws `exFailedToDecompressNode`, no empty `front()`, process alive with assertions enabled. |
| T06 `short_node_record_throws` | Compressed payloads with actual/advertised lengths 1, 2, 3: decoder throws `exFailedToDecompressNode` before a first-word read. |
| T07 `truncated_node_offsets_array_throws` | Internal marker with a short `4+(m+1)*4` array, including one-byte-short: decoder throws before separator-pointer formation. |
| T08 `unterminated_separator_throws` | Correct child array with missing final NUL, empty/doubled separator, and missing intermediate NUL: decoder throws. A complete exact separator region succeeds. |
| T09 `absurd_leaf_entry_count_throws` | Four-byte/small leaf with count greater than `m`, including `0xfffffffe`: decoder throws without a count-sized vector allocation. |
| T10 `leaf_framing_not_exact_throws` | Missing chain-size word; declared chain one byte past end; trailing bytes after the last chain; count/consumption disagreement: all throw at node validation. |
| T11 `zero_size_chain_throws` | Exact `W32(1)+W32(0)`: node decoder rejects before a chain can reach the request's `[0]`. Retain the independent call-site `empty()` defense. |
| T12 `truncated_inner_chain_record_throws` | Outer framing fits, inner word/prefix terminator missing or offset short by 1–4 bytes; include the review's 12-byte leaf with `chainSize=4`: decoder throws before traversal/string construction. |
| T13 `out_of_range_node_offset_throws` | Offsets at EOF, past EOF, and leaving 1–7 header bytes: node exception; include a root at offset zero control that succeeds. |
| T14 `oversized_btree_size_rejected` | `m=0`, `BtreeMaxElements+1`, `UINT32_MAX`: predicate false/inert open; structurally valid controls at `m=1` and `BtreeMaxElements` accepted. |
| T15 `max_node_ceiling_rejects_before_allocation` | Tiny compressed payload with advertised `U > uint64_t(C)*1032+64`, including `UINT32_MAX`: reject before output-sized allocation. Assert wide arithmetic using a separately framed near-`UINT32_MAX` C/EOF case. No fixed-size cap may satisfy this test instead. |
| T16 `builder_large_node_reads_back` | Exact large-chain builder recipe: record exceeds 16 MiB, opens, checked query returns all 1,500,000 offsets in order. This is a positive acceptance test. |
| T17 `next_leaf_internal_node_throws_not_exit` | Padded internal-target fixture via a real matching prefix request: finishes, has non-empty error, child process exits normally. Bulk helper invocation throws `exCorruptedChainData`; no `exit(1)`. |
| T18 `self_child_node_cycle_throws` | Root structurally valid and predicate true; selected child points back to itself. Checked lookup and both bulk descents throw `exCorruptedChainData` within a 5-second child-process deadline. |
| T19 `two_node_cycle_throws` | Two structurally valid internal nodes reference each other. Same lookup/bulk behavior and deadline; no reliance on a global request counter. |
| T20 `zero_size_chunk_throws` | Legal table points at `{0,0}`: Reader construction succeeds, `getBlock` throws `exFailedToDecompressChunk`; assertions-enabled/sanitized child remains alive. |
| T21 `next_leaf_internal_node_from_prefix_walk_throws` | Exact 130-word gap query and padded root: exposed lookup itself throws `exCorruptedChainData`, before returning any pointer to `readChain`. The matching-prefix route alone cannot satisfy this row. |
| T22 `next_link_to_empty_leaf_throws` | Empty target via each of the four advance routes: helper rejects with `exCorruptedChainData`; request route reports it, target's `readChain` entry count stays zero, ASan reports no read at `data()+4`. A `_data` row must also cover a trailer that is absent or only 1–3 bytes long: the read fails with `File::exReadError`, the caller's buffer and next-link state are unchanged, and no `readChain()` entry occurs. |
| T23 `reopen_valid_then_invalid_then_valid` | Cache tree A; open invalid info (including same-file and different-file variants); open valid tree B. Cache is empty/unloaded after both opens; invalid state cannot return A, final query returns B only. Use narrow test-only state observation if needed. |
| T24 `default_constructed_is_unopened` | Each throwing entry reports `exIndexWasNotOpened`; public `findArticles` logs/returns empty. It must not report `exIndexInfoUnusable` or dereference an uninitialized mutex. |
| T25 `every_entry_point_guarded_after_invalid_open` | Exercise all seven existing entries plus new checked lookup/helper. Public catching lookup logs/returns empty; all other exposed entries throw `exIndexInfoUnusable` before touching file or supplied pointer/output. |
| T26 `shared_file_position_restored_and_no_deadlock` | Valid and corrupt root probes restore a nonzero saved position; call while already holding the recursive mutex; then coordinate two threads doing validation and traversal on one file/mutex. Results correct and no deadlock. Assert position within the lock immediately after each probe; do not compare after another thread has moved it. |
| T27 `internal_node_consumes_exact_separator_region` | Exactly `m` separators succeeds; extra separator/trailing byte/fewer separators fail. Verify every internal-node field read is unaligned-safe under UBSan. |
| T28 `compressed_record_bounds_use_wide_arithmetic` | C exceeds bytes remaining, truncated eight-byte header, forged large C, and nonzero offset near 32-bit boundaries in a sparse fixture where supported: reject using subtraction before allocation/seek. Predicate restores position on these failures. |
| T29 `incremental_inflate_checks_stream_and_length` | Invalid zlib data/checksum, truncated stream, unused compressed tail, actual output shorter/longer than U, and no-progress ending: reject. High-ratio valid leaf succeeds. Successful small valid node is byte-identical to builder payload. |
| T30 `corrupt_descendant_rejected_after_sane_root` | Root predicate true, selected child contains each representative shape defect from T06–T12; checked lookup/bulk traversal fail safely and a real request reports the error. Root-only validation is insufficient. |
| T31 `empty_root_allowed_empty_child_rejected` | Production empty tree succeeds; sane internal root whose selected child is a four-byte empty leaf fails in all three descent paths. No empty target yields a chain pointer. |
| T32 `leaf_link_cycles_terminate` | One-leaf self link and two-leaf cycle via scan helper, prefix request, `findArticleLinks`, and `getHeadwordsFromOffsets`: error within deadline. Prefix lookup's visited state must survive into the continuing scan. |
| T33 `bulk_scan_more_than_4096_leaves_succeeds` | Long-scan fixture: `findAllArticleLinks`, `getAllHeadwords`, `findArticleLinks`, and offset-headword scan return all expected results beyond leaf 4,096. A separate prefix/suffix retry control may revisit the root with a new state and succeeds. |
| T34 `every_primary_producer_routes_bad_info_to_rebuild` | One data row for each of the 16 producer-file rows in §6, including FTS: real guard accepts valid header/root and rejects unchanged-identity bad main pairs. An open/short header also routes to rebuild. Invoke the guard used by its branch, not only the shared predicate. |
| T35 `every_secondary_tree_routes_bad_info_to_rebuild` | Keep main pair valid; corrupt each optional ZIP, each ZIM/SLOB resource pair, each of two MDD volume pairs, and the separate FTS pair in turn. Owning real guard rejects; good controls accepted. Test optional `{0,0}` absence, half-present invalid pair, and required empty resource root. |
| T36 `mdd_table_parsing_is_bounded` | Two variable-length name records; invalid table offset/count, count/source mismatch, truncated length/name/pair, length consuming pair bytes, and zero-volume case. Correct second record is still parsed after probing the first root, pinning position restoration. Deferred-init parser also rejects malformed input. |
| T37 `zip_secondary_pair_uses_owning_index_file` | Different valid bytes at identical offsets in owner index and ZIP/source file. Real owner guard must validate its persisted tree against the owner, and ZIP loading uses the separate source stream. ZipSounds's two consumers use one validated pair. |
| T38 `chunk_table_and_block_ranges_are_bounded` | Bad table offset/count bytes, chunk index, chunk record offset, C past EOF, bad stream/exact size, and block offset beyond output fail with specified chunk/address exception. Offset exactly at end and valid empty zlib chunk are accepted without dereference; non-empty record consumers reject the empty suffix. |
| T39 `writer_oversized_chunk_block_reads_back` | Production 65,537-byte block: readback byte-identical. A larger highly compressible block also succeeds below its deflate bound. Neither `U<=65536` nor a ZIP/chunk conflation is allowed. |
| T49 `mdx_stylesheet_records_are_bounded` | Stylesheet table with (a) zero-length begin/end, (b) a length running past EOF, (c) a count larger than the remaining file, (d) a record whose bytes contain no NUL: no `front()` on an empty buffer, no read past EOF, conversion uses the explicit length, and the failure reaches the dictionary-init/request error. |
| T50 `mdx_description_is_bounded` | Chunk-held description with `descriptionSize` shorter than the chunk, and one that is exactly zero: the reported text honours `descriptionSize` and no unbounded `std::string` construction occurs. |
| T40 `indexed_zip_stored_entries_are_size_checked` | Actual method-0 ZIP: empty and 65,537-byte entries succeed; wrong C/U, local-header/name/extra truncation, source offset/payload past EOF fail and clear prior output. Test through `IndexedZip::loadFile`, not only a length helper. |
| T41 `indexed_zip_deflate_is_incremental_and_exact` | Actual method-8 raw stream: empty and large valid content succeed; malformed/truncated stream, advertised size mismatch, unused compressed tail, expansion bound violation, and unsupported method return false/clear output. An owning resource request reports malformed referenced data. |
| T42 `mdd_record_slice_is_bounded` | Real MDD loader: negative fields, short `RecordInfo`, compressed source region out of range, `recordOffset>D`, `recordSize>D-offset`, and arithmetic extremes fail. Zero record at D and exact nonzero slice succeed; no zero-size `front()`/copy. |
| T43 `mdx_article_record_slice_is_bounded` | Same boundary matrix in the real MDX article loader/request, proving the MDD-only patch cannot satisfy it. Source parser's plain/zlib and supported LZO paths retain their own codec/error rules. |
| T44 `prefix_worker_reports_error_and_finishes` | Inert dictionary and sane-root/corrupt-descendant dictionary through real `prefixMatch`/stem request: one completion signal, non-empty error, no success-only partial matches, runnable exits. Sane no-match and cancellation controls retain normal semantics. |
| T45 `fts_worker_reports_storage_failures` | Real FTS request: invalid FTS pair, corrupt descendant, malformed posting count, truncated posting list, combined-index variant. Each finishes with a non-empty error; it cannot succeed by the legacy catching lookup returning empty or by an unrequested full-scan fallback. |
| T46 `chunk_record_consumers_check_each_field` | Data rows through real SoundDir, ZipSounds, Stardict, DSL, GLS, XDXF, BGL, EPWING and FTS readers: byte-boundary record recipe, sized-string/abbreviation/posting counts, and positive exact-size controls. Each malformed case reaches a request error or specified reader exception, not an out-of-range copy/string scan. |
| T47 `resource_workers_contain_lengths_and_redirect_cycles` | Real BGL resource-list/decompressor, sound resource records, and MDX/MDD redirect consumers: forged filename/resource sizes, missing record fields, empty redirect, self/two-target redirects, and malformed compressed resources. Requests finish with errors; valid empty/large resources succeed. Include cached-resource redirect route. |
| T48 `remaining_worker_closure_handles_empty_and_short_data` | Real Aard, SDict, Dictd, LSA, ZIM and SLOB request/reader rows plus image/result-copy paths in §7: zero-length results, short WAV/source/cluster/bin records, overflowed length calculations, and positive exact-boundary controls. Requests finish; no `front()` assertion, sanitizer finding, unbounded redirect, or exception escaping a runnable. |

### 8.4 Verification gates and falsification

Run the ordinary aggregate using its documented mode (`tests/tests.pro:5`):

```sh
qmake6 tests/tests.pro -o tests/Makefile
make -C tests -j$(nproc)
QT_QPA_PLATFORM=xcb xvfb-run -a timeout 900 make -C tests check
```

Require all 19 suites. Compile-closure mode only produces an archive and has no `check` target; it is not equivalent acceptance (`tests/tests.pro:26`). Follow the shared target-specific object/MOC/UI/RCC paths (`tests/testsupport.pri:65`) and do not mix sanitizer/ordinary objects.

Build the application and suite with Arch's actual configured compiler flags, retaining `_GLIBCXX_ASSERTIONS`; disabling assertions is a failing workaround. Run a separate standalone ASan/UBSan build/invocation of the new target, with isolated output directories and both compile/link sanitizer flags. Include T05–T12, T17–T22, T28–T32, T38, and T40–T50; sanitizer errors or killed/timed-out children fail the gate. Test subprocesses must check normal exit status as well as diagnostic text. Keep crash demonstrations out of the aggregate process.

Use isolated temporary source copies for negative controls: remove the non-empty-next-leaf condition (T22 must fail), omit the prefix-walk helper call (T21 must fail), omit one secondary guard (its T35 row must fail), change the chunk check to `U<=65536` (T39 must fail), restore a 4,096 traversal cap (T33 must fail), and route FTS through the catching lookup (T45 must fail). These prove tests reach the named production mechanism. Do not mutate the active checkout to run a probe or claim a mutant was killed by a baseline-failing test.

The following are separate acceptance runs requiring the operator's existing local corpus; they are not counted among the 50 hermetic slots:

- Enumerate every persisted pair: 20 top-level files in the reviewed corpus, every MDX volume pair, every optional ZIP/ZIM/SLOB resource pair, and every `_FTS` file. Record file identity, owner, pair fields, result, and before/after SHA-256. Require 17 accepted top-level files and exactly three rejected foreign SoundDir files; embedded/FTS valid pairs remain accepted. A different corpus requires an explicit inventory delta, not silently forcing those historical counts.
- Copy the configuration and index tree into a scratch **HOME override** (`HOME=<tmpdir>`, plus
  `XDG_CONFIG_HOME`/`XDG_CACHE_HOME`/`XDG_RUNTIME_DIR` under it) and run the lookup with the built
  executable and scratch copies only. Portable mode is NOT usable for this gate: `main.cc:477`
  executes `cfg.soundDirs.clear()` and `sources.cc:135-147` disables the sound-directory controls, so a
  portable launch can never rebuild or exercise the SoundDir stores whose corruption is the original
  defect. Require a normal live process, the three foreign files rebuilt with this tree's six-field
  header and valid roots, the other 17 top-level index files byte-identical, successful affected sound
  lookups, and no new process core. Do not delete or rewrite the user's active index directory to
  demonstrate the fix; the override keeps every write inside the scratch HOME.
- Produce the final worker-hit classification from §7, and compile all modified producer/codec feature configurations. A successful small unit suite does not replace compiling the real protected helper call from `findMatches()` or the feature-conditional readers.

## 9. Compatibility

No header size, persisted field order/width, version expression, filename convention, or compression wrapper changes. In particular do not bump `BtreeIndexing::FormatVersion`, per-format versions, FTS versions, or packaging versions to hide incompatible validation. The foreign SoundDir file is rebuilt because its metadata cannot describe its file; an arbitrary difference between total file size and `sizeof(IdxHeader)` is not a valid discriminator.

Large-chain output from the existing builder must remain readable, including the reviewed 19,500,008-byte node. The deflate inequality uses `uint64_t(C)*1032+64`, and incremental growth uses actual output. The 65,536-byte output window does not impose an allocation-total ceiling. There is no surviving `MaxNodeBytes` identifier/requirement from an older design revision.

The chunk address's 16-bit offset does not limit the length of a block that begins there. Preserve oversized blocks, empty allocated blocks with a real stream, and one-past-end zero-length block starts. ZIP resources have independent framing and may exceed a chunk's nominal flush size. A valid zero-length article/resource is not corruption solely because its output vector is empty; zero-size compressed node records and absent compressed streams remain corrupt.

Repeated-offset detection replaces the design's arbitrary 4,096-node budget. Valid bulk scans must traverse more than 4,096 leaves; per-query restart state permits repeated root visits for suffix retries. Retain cancellation checks in long successful scans. This does not attempt semantic proof of balancing, separator ordering, or validity of an article address in its separate source file beyond the range checks at the consuming reader.

Keep public `findArticles()` as the existing logging/empty-result wrapper. Use the new throwing checked lookup within request owners that need to report errors. Existing bulk APIs already propagate errors and retain that behavior (`btreeidx.cc:1263`, `btreeidx.cc:1281`). Neither a never-opened object nor a failed reopen may acquire stale valid data. Other dictionaries continue serving requests when one request reports corrupt storage.

## 10. Traceability matrix

`reports/` below means the PM run's report directory for `pm-run-20261030-030606-0654b80a`, not a new repository report to create. Review 1 and review 3 did not return reviewer artifacts; the recorded PM dispositions adjudicate their findings. Review 4's incremental verdict was `PENDING`, with the blocker closed by design v5 and its recorded disposition; this specification does not relabel it as an independent clean approval.

| Requirement/finding | Resolution in this specification | Verification |
|---|---|---|
| Operator: shared predicate in `BtreeIndexing` | Public static `BtreeIndex::indexInfoIsSane`; all producer guards call it (§2, §3.3, §6). | T01–T04, T34–T37. |
| Operator: inert open, not constructor exception | Reset/attempt/commit state machine and separate never-opened error (§4). | T04, T23–T25, T44. |
| Operator: new suite and full hardening | New target plus 19-suite aggregate; traversal, storage, resource, and worker closure in scope (§7, §8). | T01–T50 and application/sanitizer/sweep gates. |
| Design D1, record framing | Range-before-read/allocation, wide deflate bound, exact incremental inflate (§3.1). | T05, T06, T13, T15, T28, T29. |
| Review 1 finding 1a: predicate/open recursion | Probe initializes members directly; validator never calls `openIndex` (§3.3). | T02, T26. |
| Design D2, fan-out bound | `btreeMaxElements` outside `[1, BtreeMaxElements]` is not sane; open stays inert (§3.3, §4). | T14, T24. |
| Review 1 findings 1b/1c and ASan probes | Minimum word, child-array fit, exact separators, exact outer and inner chain framing (§3.2). | T06–T12, T27, T30. |
| Review 1 findings 2a/3b: omitted producers and stale corpus count | Enumerate 16 producer files plus every secondary pair; 17 accepted/3 foreign in the historical 20-file corpus (§6, §8.4). | T34–T37, pair sweep. |
| Review 1 finding 2b: worker `exit(1)` | Replace all four leaf advances with one protected checked helper; request error handling (§5, §7). | T17, T21, T22, T44. |
| Review 2 BLOCKER 1: 16 MiB cap rejects a valid 19,500,008-byte node | Remove fixed caps; preserve large builder chain; use format-derived 1032 factor (§3.1, §8.2). Older 256 MiB proposal is likewise rejected. | T15, T16, T29. |
| Review 2 BLOCKER 2: inner chain overread and empty chain | Whole-chain framing before traversal plus call-site empty-chain defense (§3.2, §5). | T10–T12, T30. |
| Review 2 BLOCKER 3: secondary opens contained but never rebuilt | Validate main-file MDD table, optional ZIP pairs, resource pairs and separate FTS file in owning guards (§6). | T35–T37; secondary-guard negative control. |
| Review 2 BLOCKER 4: workers still abort outside btree | Harden chunks, ZIP, MDD/MDX and the complete worker closure (§3.4, §7). | T20, T38–T48, assertion/sanitizer gates. |
| Review 2 MAJOR 1: validly framed cycles | Per-traversal repeated-offset detection shared across descent/advance (§5), with long-scan correction below. | T18, T19, T32, T33. |
| Review 2 MAJOR 2/4: reopen cache and default initialization | Reset first, pointers saved on attempted open, in-class `indexUsable=false`, initialized constructor (§2, §4). | T23–T25. |
| Review 2 MAJOR 3: implementation/test gap at chain checks | Exact failing byte fixtures and descendant tests; no root-only apparent coverage (§8.1–§8.3). | T11, T12, T30. |
| Review 2 MAJOR 5: file position and locking | One recursive-mutex save/decode/restore window; failure cleanup and concurrency contract (§3.3). | T26, T36. |
| Review 2 MINORs: result/catch behavior and practical test linkage | Preserve catching `findArticles`, explicitly document propagating bulk APIs, add checked API for request reporting; link actual sources and owner closures (§2, §5, §8.1). | T04, T24, T25, T34–T48; full build. |
| Review 3 adjudication: unguarded prefix advance | Prefix lookup uses the same helper and state, rather than relying on the request's later check (§5). | T21 and its helper-call negative control. |
| Review 3 adjudication: arbitrary size-limit remnants | No `MaxNodeBytes`, including the design compatibility sentence that still mentions it (§9). | T16, T29, source review. |
| Review 4 BLOCKER 1: empty leaf advances return `data()+4` past end | Helper requires `1 <= first <= m` and rejects before returning a pointer (§5). | T22, target `readChain` counter, non-empty-condition negative control. |
| Review 4 MAJOR 1: helper accessibility | Protected declaration, real `BtreeWordSearchRequest` call compiled (§2.2). | Full production/suite compile. |
| Review 4 MAJOR 2: internal-root fixture short read masks fault | Append readable trailer padding after the last internal root; retain matching and gap routes (§8.2). | T17, T21. |
| Review 4 zlib bound analysis/probes | Use factor 1032, not an estimated 516/1,000 cap; exact uint64 formula and high-ratio positive stream (§3.1). | T29, T15. |
| Inspection correction C1: design D5.3 caps chunks at 65,536 | Override that clause. Blocks may exceed the chunk threshold, as explicitly documented (`chunkedstorage.hh:17`) and implemented by whole-block append (`chunkedstorage.cc:47`). Validate actual stream/ranges (§3.4, §9). | T39 and cap negative control. |
| Inspection correction C2: design D6 assumes traversal depth bounds scan length | Override `MaxTraversalNodes=4096`. Bulk methods scan linked leaves (`btreeidx.cc:1340`, `btreeidx.cc:1470`); use visited offsets and allow more than 4,096 leaves (§5, §9). | T32, T33 and count-cap negative control. |
| Inspection correction C3: empty chunk versus absent stream | Refine D5.3's blanket nonzero-U clause: writer tracks allocated empty blocks (`chunkedstorage.hh:56`); allow valid empty zlib output, reject C=0 (§3.4). | T20, T38, T48. |
| Inspection correction C4: ZIP is not chunk storage | Replace D5.5's `ChunkMaxSize` suggestion with ZIP/source/deflate properties; valid large/empty entries remain accepted (§3.4). | T40, T41, T47. |
| Inspection correction C5: logging alone does not report request failure | Existing prefix/FTS catches and public lookup swallow errors (`btreeidx.cc:111`, `btreeidx.cc:424`, `ftshelpers.cc:1332`). Checked lookup plus outer request reporting supplies the promised dictionary-local errors (§2.2, §7). | T44, T45, T47, T48 and catching-lookup negative control. |
| Inspection correction C6: design D5 missed MDX-adjacent metadata (spec review 1 BLOCKER 1) | Add the stylesheet/description paths the worker closure did not name (`mdx.cc:420–437`, `mdx.cc:928–943`): bounded lengths, zero-length skips, explicit-length conversion, `descriptionSize`-bounded descriptor. | T49, T50 and the §7 final sweep. |
| Inspection correction C7: portable launch cannot exercise SoundDir (planner-discovered) | Design/spec §8.4 named the portable-directory path for acceptance, but `main.cc:477` clears `cfg.soundDirs` and `sources.cc:135-147` disables them there, so the gate could never rebuild the three foreign stores. Replace with an isolated scratch **HOME override**. | Task 27 acceptance; no test slot (local-corpus gate). |
| Spec review 1 MAJOR 1: truncated trailer untested | T22 gains a `_data` row for an absent or 1–3-byte trailer; `File::exReadError`, unchanged caller state, no `readChain{}. | T22. |
| Structural-only chain validation | Resolve “minimum seven bytes” as ordinary-word commentary, not an additional semantic rule: field framing itself is two NULs and four bytes (`btreeidx.cc:961`); do not reject otherwise complete records solely for an empty word (§2, §3.2). | T12 positive framing controls; no unrequested semantic validation. |
| Existing FTS/optional-ZIP conventions | Preserve FTS version sum and `{0,0}` optional ZIP absence while still checking every present pair (§6). Required ZIM/SLOB empty trees retain a real root. | T35, T37. |
| No source/test/design edits by the specification drafting task | This deliverable is only this specification. Implementation and tests are future work under the ordered contract below. | Drafting diff/file inventory verification. |

## 11. Implementation order

1. Add the new test target/closed inventory entry, production-linked probe fixtures, foreign header, and sane controls. Establish the original abort/shape/cycle failures in isolated subprocesses/copies; confirm fixtures reach their named path. Do not run an intentional pre-fix process kill inside the aggregate runner.
2. Add the exception, constructor/state initialization, protected traversal declarations, checked lookup boundary, shared predicate with position restoration, and inert `openIndex`. Compile the real prefix request's protected-helper access as soon as the helper exists. Run T01–T04 and T23–T26.
3. Implement bounded incremental node decoding and exact node/inner-chain shape checks. Preserve file position after payload and strong publication semantics. Run T05–T16 and T27–T31, including the large positive builder fixture and high-ratio stream.
4. Implement per-traversal repeated-offset state and the shared non-empty-leaf helper; wire all three descents and four advances, remove the worker `exit(1)`, and add the empty-chain defense. Run T17–T22 and T32–T33; prove both internal-target routes and all empty-target routes.
5. Harden chunk table/record/block handling, ZIP framing/raw deflate, and MDX/MDD record slices/source maps. Preserve oversized/empty valid contents. Run T20 and T38–T43 plus affected existing suites.
6. Extend every primary/secondary/FTS guard in §6, including bounded MDX table parsing and deferred-init rechecks. Keep existing versions and absent-pair semantics. Run the actual producer/secondary tests T34–T37.
7. Complete every worker/record-reader row in §7, switch error-sensitive lookups to the checked API, and add missing request catches/error strings/empty-output handling. Run T44–T48 and re-derive the worker inventory. Verify runnable completion/semaphore behavior on failures and cancellation.
8. Run the 19-suite aggregate, real application builds with Arch assertions and supported producer features, standalone sanitizers, and isolated negative controls. Then run the corpus pair/hash sweep and isolated portable lookup acceptance. Resolve any valid-index rejection by correcting validation; do not bump versions, weaken assertion flags, or shrink the compatibility fixtures.
9. Review the final diff against the invariant, producer, traversal, worker, and traceability tables. Report exact gate results and corpus limitations. Implementation is complete only when every required row has production evidence; a safely inert but unrebuildable dictionary or a logged-but-unreported worker failure is incomplete.

### 11.1 Executable commands per step

Suite paths below assume `GD_SRC` is the checkout root and `T=tests/tst_btree_index/tst_btree_index`.

Every command builds **outside** the checkout; no step writes build output into `$GD_SRC`.

```sh
# --- per-step: one isolated build dir per step, then only the affected slots ---
step() {                      # step() <dir> <slot>...   -- builds in /tmp/gd-build/<dir>
  local d=/tmp/gd-build/$1; shift
  rm -rf "$d" && mkdir -p "$d"
  qmake6 "$GD_SRC/tests/tst_btree_index/tst_btree_index.pro" -o "$d/Makefile" || return 1
  make -C "$d" -j"$(nproc)" || return 1
  QT_QPA_PLATFORM=xcb xvfb-run -a "$d/tst_btree_index" "$@"
}

# step 2 slots
step s2 builds_and_reads_back foreign_sounddir_header_is_not_sane \
        insane_index_info_is_inert open_index_terminates

# step 3 slots
step s3 zeroed_node_record_throws short_node_record_throws \
        truncated_node_offsets_array_throws unterminated_separator_throws \
        absurd_leaf_entry_count_throws leaf_framing_not_exact_throws \
        builder_large_node_reads_back max_node_ceiling_rejects_before_allocation

# step 4 slots (both internal-target routes, every empty-target route)
step s4 next_leaf_internal_node_throws_not_exit \
        next_leaf_internal_node_from_prefix_walk_throws \
        next_link_to_empty_leaf_throws self_child_node_cycle_throws \
        leaf_link_cycles_terminate

# step 6 slots (producer + secondary rebuild wiring, incl. MDX metadata)
step s6 every_primary_producer_routes_bad_info_to_rebuild \
        every_secondary_tree_routes_bad_info_to_rebuild

# step 7 slots (worker closure, incl. the MDX metadata paths added by review 1)
step s7 mdx_stylesheet_records_are_bounded mdx_description_is_bounded \
        mdx_article_record_slice_is_bounded mdd_record_slice_is_bounded \
        fts_worker_reports_storage_failures prefix_worker_reports_error_and_finishes

# --- standalone sanitizer build (isolated objects; never mixed with the ordinary build) ---
mkdir -p /tmp/gd-asan && cd /tmp/gd-asan
qmake6 "$GD_SRC/tests/tst_btree_index/tst_btree_index.pro" -o Makefile \
    "QMAKE_CXXFLAGS+=-fsanitize=address,undefined -fno-omit-frame-pointer -g" \
    "QMAKE_LFLAGS+=-fsanitize=address,undefined"
make -j"$(nproc)" || exit 1
set -o pipefail                       # keep the test process status through tee
ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
    ./tst_btree_index 2>&1 | tee /tmp/gd-asan/sanitizer.log
rc=${PIPESTATUS[0]}; [ "$rc" -eq 0 ] || { echo "sanitizer run failed rc=$rc"; exit "$rc"; }
```

Each step's named rows must be green before the next step starts. Feature-conditional producers
(`MAKE_ZIM_SUPPORT`, `NO_EPWING_SUPPORT`, `MAKE_EXTRA_TIFF_HANDLER`, `no_ffmpeg_player`) are
compiled with the same `.pro` switches the packaged build uses (`goldendict.pro`); a step is not
complete until the producers it touched compile in each configuration the package enables.

### 11.2 Negative controls (isolated copies only)

Each control runs in a disposable copy so the active checkout is never mutated:

Each control is scripted identically; `mutate()` must prove the edit applied, and the build is
regenerated inside the disposable copy so no stale object can mask the mutation:

```sh
control() {                   # control <n> <file> <old> <new> <slot-that-must-fail>
  local n=$1 f=$2 old=$3 new=$4 slot=$5 d=/tmp/gd-mutant-$1
  rm -rf "$d" && cp -a "$GD_SRC" "$d" && cd "$d" || return 1
  python3 - "$f" "$old" "$new" <<'EOF'
import sys; f,old,new=sys.argv[1:4]; s=open(f).read()
assert old in s, "mutation anchor absent: the source no longer matches this spec"
open(f,"w").write(s.replace(old,new,1))
EOF
  git -C "$d" diff --quiet && { echo "mutant $n did NOT apply"; return 1; }
  rm -rf /tmp/gd-build/m$n && mkdir -p /tmp/gd-build/m$n
  qmake6 "$d/tests/tst_btree_index/tst_btree_index.pro" -o /tmp/gd-build/m$n/Makefile || return 1
  make -C /tmp/gd-build/m$n -j"$(nproc)" || return 1
  QT_QPA_PLATFORM=xcb xvfb-run -a /tmp/gd-build/m$n/tst_btree_index "$slot"
  local rc=$?
  [ "$rc" -ne 0 ] || { echo "mutant $n SURVIVED $slot"; return 1; }
  echo "mutant $n killed by $slot"
}

control 1 btreeidx.cc "if ( !first || first == 0xffffFFFF )" "if ( false )" \
        next_link_to_empty_leaf_throws
control 2 btreeidx.cc "readNextLeaf(" "readNextLeafDisabled(" \
        next_leaf_internal_node_from_prefix_walk_throws
control 3 sounddir.cc  "indexInfoIsSane" "true" \
        every_primary_producer_routes_bad_info_to_rebuild
control 4 chunkedstorage.cc "ChunkMaxSize" "65536 /*restored cap*/" \
        writer_oversized_chunk_block_reads_back
control 5 btreeidx.cc   "visitedOffsets" "visitedOffsetsUnused4096" \
        bulk_scan_more_than_4096_leaves_succeeds
control 6 ftshelpers.cc "findArticlesChecked" "findArticles" \
        fts_worker_reports_storage_failures
control 7 btreeidx.cc   "chainSize != 0" "true" \
        truncated_inner_chain_record_throws
```

The exact mutation anchors above are illustrative of the required *shape*: the implementer must
replace each with the real expression in the finished code and confirm `git diff` shows the change
before building. A control that fails to apply, or that passes, fails the gate.

| Control | Mutation | Must fail |
|---|---|---|
| 1 | remove the non-empty-next-leaf condition | `next_link_to_empty_leaf_throws` |
| 2 | remove the prefix-walk helper call | `next_leaf_internal_node_from_prefix_walk_throws` |
| 3 | remove one secondary guard row from §6 | that producer's `every_secondary_tree_routes_bad_info_to_rebuild` data row |
| 4 | restore `U <= 65536` as the chunk bound | `writer_oversized_chunk_block_reads_back` |
| 5 | restore a 4,096-traversal cap | `bulk_scan_more_than_4096_leaves_succeeds` |
| 6 | route FTS through the catching lookup | `fts_worker_reports_storage_failures` |
| 7 | skip the inner-chain record walk | `truncated_inner_chain_record_throws` |

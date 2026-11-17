# Scan popup crash on an empty resource payload

Status: revised after design review rounds 1-4
Date: 2026-11-16
Run: pm-run-20261116-203148-068023b6
Target: `refs/heads/master` at `30a7111d750ea1aac214b208172640e0dd10befd`

## Background

The scan popup aborted the whole application (`SIGABRT`, coredump) while a dictionary
article was being displayed. The installed binary (`/usr/bin/goldendict`, package
`2:1.5.4.r9.g30a7111d-1`) was built from the pinned commit, so the defect is present
in `master`.

The dumping thread's frames were:

```
abort
__glibcxx_assert_fail
goldendict + 0xe2af1                     # std::vector<char>::front()
Mdx::MddResourceRequest::run()           # run + 0x348
QThreadPool / QRunnable
```

## Confirmed diagnosis

The crashing access is an empty-vector `front()` inside `File::loadFromFile()`
(`file.cc:57-66`), reached from `MddResourceRequest::run()`'s local-file branch:

```cpp
File::Class f( n, "rb" );
f.seekEnd();
data.resize( f.tell() );      // 0 for an empty file
f.rewind();
f.read( &data.front(), data.size() );   // front() on an empty vector
```

Evidence, independent of the stack text:

- `run + 0x342` (`0x3571a2`) calls `File::loadFromFile`; the recorded frame
  `run + 0x348` (`0x3571a8`) is exactly the return address of that call.
- `File::loadFromFile` branches at `0x1bc1e0` to its cold assertion block at
  `0xe2ad1` when the vector is empty; the recorded frame `0xe2af1` is that block's
  return address.
- The assertion's own strings are `!this->empty()`,
  `/usr/include/c++/16/bits/stl_vector.h`, and
  `std::vector<char>::front()`.

Arch's `makepkg` flags include `-D_GLIBCXX_ASSERTIONS`, so this undefined access
aborts instead of reading a released buffer silently. Other threads in the dump were
inside `isolateCSS()` because the popup issues several resource requests at once;
their progress does not identify the aborting one.

### Second, independent defect of the same class

Five backends copy a stylesheet that `isolateCSS()` has legitimately reduced to zero
bytes (comments only, or `@page` rules only, which GoldenDict deliberately drops) into
the payload with the same `resize`-then-`front()` idiom:

`mdx.cc:937-940`, `gls.cc:1459-1462`, `stardict.cc:1753-1756`, `slob.cc:1672-1680`,
`zim.cc:1588-1595`.

These are not the reported abort, but they abort on a comment-only or `@page`-only
stylesheet in any of those dictionary formats, in the same popup.

## Goals

- The scan popup never aborts because a resource payload is empty.
- Non-empty payloads are served byte-for-byte as before; rendering does not change.
- Every place on the article-resource path that builds or reads a payload either uses
  one tested mechanism or is proven non-empty by its own invariant.
- A regression suite fails on the pre-repair code and passes after it.

## Non-goals

- Changing what `isolateCSS()` keeps or drops (for example re-injecting `@page`).
- Changing the `hasAnyData` / `dataSize()` success-versus-missing contract.
- Article payload producers (`getArticle`): they are a separate producer family with
  their own no-article early returns. `xdxf.cc:657` is excluded because the request
  returns at `xdxf.cc:619` when both article maps are empty, and every surviving
  iteration appends a non-empty cleaner, so its result cannot be empty. The website
  article append (`website.cc:203`) is excluded because the body always carries the
  fixed wrapper and cleaner strings.
- Truncated-index I/O in BGL: `File::Class::read` throws `exReadError` before any
  payload exists, and this repair does not change that propagation. The BGL row below
  covers a fully readable record whose zlib stream is invalid or produces no output.
- Any UI change or user-visible message.

## Resource-payload defect inventory

Every row is on the resource path the scan popup drives: `ArticleSchemeHandler` asks
the producing dictionary for a `bres://` resource, and the dictionary backend builds
the payload below.

| Group | Site | Problem |
| --- | --- | --- |
| Local file load | `file.cc:57-66` | Empty file: `front()` on an empty payload. **The reported abort.** |
| Local file load | `sounddir.cc:372-384` | Hand-rolled `File::Class` load with the same shape; it will call `File::loadFromFile` instead. |
| Buffered copy | `epwing.cc:780-790` | Empty `QByteArray` copied through `front()`. |
| Stylesheet | `mdx.cc:937-940` | Comment-only/`@page`-only CSS isolates to empty. |
| Stylesheet | `gls.cc:1459-1462` | Same. |
| Stylesheet | `stardict.cc:1753-1756` | Same. |
| Stylesheet | `slob.cc:1672-1680` | Same. |
| Stylesheet | `zim.cc:1588-1595` | Same. |
| Stylesheet append | `website.cc:352-359` | Empty CSS reply appended through `front()+prevSize` when the payload is still empty. |
| Image | `xdxf.cc:1132-1163` | Empty TIFF bytes passed to `&data.front()`; empty BMP buffer copied through `front()`. |
| Image | `dsl.cc:2034-2062` | Same decode and BMP-copy shape. |
| Image | `gls.cc:1384-1414` | Same. |
| Image | `stardict.cc:1678-1708` | Same. |
| Image | `slob.cc:1686-1712` | Decodes from the non-empty resource string, but the `GdTiff` fallback at `slob.cc:1693` passes the still-empty `data` vector as a pointer; the BMP copy can also be empty. |
| Image | `zim.cc:1601-1627` | Same fallback at `zim.cc:1608` and the same BMP copy. |
| Archive resource | `slob.cc:1715-1718` | Defensive only: `slob.cc:1669` rejects an empty resource string first, so `resource` is non-empty here. |
| Archive resource | `zim.cc:1630-1633` | Defensive only: `zim.cc:1584` rejects an empty resource string first. |
| Compressed resource | `bgl.cc:1045-1078` | Empty name, compressed, or decompressed buffers read or passed to `front()`/`uncompress`; a zero-length name must still consume its trailing offset, and a valid empty stream must stay a successful empty resource. |

The article-image size probe at `dsl.cc:1001-1011` is excluded because
`dsl.cc:987` guards `imgdata` before the pointer is formed.

## Design

### `payload.hh` — the copy rule

A header-only module, dependency-free (`<cstddef>`, `<cstring>`, `<vector>`):

```cpp
namespace Payload {
/// Replaces `destination` with exactly `size` bytes from `bytes`.
/// Zero bytes is legal: the vector becomes empty and neither pointer is dereferenced.
inline void assign( std::vector< char > & destination, char const * bytes, std::size_t size );

/// Appends `size` bytes from `bytes`. Zero bytes is a no-op, including when the
/// destination is still empty.
inline void append( std::vector< char > & destination, char const * bytes, std::size_t size );
}
```

### `Dictionary::Class::setIsolatedCssPayload()`

A new public member of `Dictionary::Class`, implemented in `dictionary.cc`:

```cpp
/// Isolates `css` for this dictionary and replaces `data` with the exact UTF-8
/// bytes of the result. An empty result is legal and yields an empty payload.
void setIsolatedCssPayload( QString & css, std::vector< char > & data,
                            QString const & wrapperSelector = QString() );
```

It calls the existing protected `isolateCSS()` and then `Payload::assign()`. The five
stylesheet backends replace their three-line `isolateCSS`/`toUtf8`/copy sequence with
one call, keeping whatever `dataMutex` discipline each already has. `website.cc`
rewrites `isolateWebCSS` output and therefore uses `Payload::append` instead.

### `imagepayload.hh/.cc` — the TIFF conversion

Three functions that separate the two decoders from their composition:

```cpp
namespace ImagePayload {
/// Decodes with Qt's image readers. Returns a null QImage when none accepts the bytes.
QImage decodeWithQt( char const * bytes, std::size_t size );

/// Decodes a one-bit, single-sample TIFF with the bundled GdTiff reader.
/// Returns a null QImage for any other input.
QImage decodeWithGdTiff( char const * bytes, std::size_t size );

/// Qt first, GdTiff second; returns an empty QByteArray when neither decodes.
QByteArray tiffToBmp( char const * bytes, std::size_t size );
}
```

Callers keep their mutex and their current source: `data` in dsl/gls/stardict/xdxf,
the non-empty local resource string in slob/zim. They assign only a non-empty result:

```cpp
QByteArray const bmp = ImagePayload::tiffToBmp( source.data(), source.size() );
if ( !bmp.isEmpty() ) {
  Mutex::Lock _( dataMutex );
  Payload::assign( data, bmp.constData(), bmp.size() );
}
```

Undecodable input therefore leaves the payload exactly as before (the raw TIFF bytes
in dsl/gls/stardict, empty in slob/zim), while empty input never forms a pointer.

### `bglresource.hh/.cc` — the BGL record rules

The two BGL rules that must survive the guard are extracted so they can be executed:

```cpp
namespace BglResource {
struct DirectoryEntry { std::string name; uint32_t offset; };

/// Reads one directory record at the current index position: a uint32 name
/// length, the name bytes, then the mandatory uint32 offset. A zero-length name
/// is a valid record and still consumes its offset. Read failures propagate as
/// File::Class::read already does; a completed record has no false outcome.
void readDirectoryEntry( File::Class & idx, DirectoryEntry & entry );

/// Decompresses one resource block into `data`, which is resized to
/// `advertisedSize` first (a one-byte scratch is used while the advertised size
/// is zero). Success requires Z_OK and an actual output length equal to
/// `advertisedSize`. A valid stream that produces zero bytes is success with an
/// empty payload; empty or corrupt input returns false.
bool decompress( std::vector< char > const & compressed, std::size_t advertisedSize,
                 std::vector< char > & data );
}
```

`BglResourceRequest::run()` uses them for its directory loop and resource decode; the
name comparison and `idx.seek` remain at the call site. No exception handling is
added around record I/O, so truncated-index behaviour is unchanged: `readDirectoryEntry`
lets `File::Class::read` throw exactly as today, and callers do not catch it.

### Remaining sites

- `file.cc:57-66`: resize from the file size, then read only when the vector is
  non-empty. `sounddir.cc:372-384` drops its hand-rolled version and calls
  `File::loadFromFile`, inheriting the guard.
- `epwing.cc`: `Payload::assign( data, buffer.constData(), buffer.size() )`.
- `slob.cc`/`zim.cc` archive resources: `Payload::assign( data, resource.data(),
  resource.size() )` as defensive cleanup.
- `bgl.cc`: the directory and decompression bodies move to `BglResource`, which
  applies the two rules above; the request keeps its existing success flag.

### Empty-payload behaviour, exactly

No `hasAnyData` line and no reader semantics change. The backend-specific outcomes:

| Case | Outcome | `dataSize()` |
| --- | --- | --- |
| MDD stylesheet transformed to empty | `hasAnyData` was already set at `mdx.cc:900` from the raw resource. | `0` |
| MDD request, empty local file | `File::loadFromFile` returns empty; the request's existing `data.size() > 0` test leaves the flag unset. | `-1` |
| GLS/StarDict/SLOB/ZIM/website stylesheet transformed to empty | `hasAnyData` is set after the payload block as today. | `0` |
| SLOB/ZIM initially empty resource | Rejected by `slob.cc:1669` / `zim.cc:1584` before payload work. | `-1` |
| SoundDir empty file | `DataRequestInstant(true)` holds an empty payload. | `0` |
| EPWING empty cache file | The cache file is read; `hasAnyData` is set after the copy. | `0` |
| DSL/GLS/StarDict/XDXF image, empty bytes | Decoding is skipped, the payload stays empty, success is set after the block. | `0` |
| DSL/GLS/StarDict/XDXF image, non-empty but undecodable | The original raw bytes remain in the payload (the assignment is skipped). | original size |
| SLOB/ZIM image, empty resource | Rejected before payload work. | `-1` |
| SLOB/ZIM image, non-empty but undecodable | No BMP is assigned; the payload stays empty; success is set after the block. | `0` |
| XDXF ZIP entry, empty stored bytes | `IndexedZip::loadFile` clears the payload and reports success (`indexedzip.cc:78-81`); XDXF then publishes success at `xdxf.cc:1167`. No new seam is involved. | `0` |
| SLOB/ZIM archive entry, empty stored bytes | Unreachable: the resource string is rejected first. | `-1` |
| BGL valid stream producing zero bytes | `BglResource::decompress` succeeds; success is published. | `0` |
| BGL invalid stream in a fully readable record | `BglResource::decompress` fails; success is not published. | `-1` |
| BGL truncated index record | Unchanged: `File::Class::read` throws before any payload exists. | unchanged |

The specification pins each row to its source line before implementation.

### Alternatives considered

1. Inline guards at each site only — smallest diff, but the reported semantics would
   be untestable in the no-display suites and six copies of the rule invite a
   seventh. Rejected.
2. A safe wrapper type for every request payload — changes a widely used container
   and every producer. Out of proportion. Rejected.
3. Shared seams (chosen): one tested copy rule, one tested stylesheet transform, one
   tested image conversion, one tested BGL record module, and a guarded file loader;
   backends keep one-line calls.

## Application build registration

`goldendict.pro` enumerates its sources manually (`goldendict.pro:444` starts the
list), so the design adds:

- `payload.hh` to `HEADERS`;
- `imagepayload.hh` to `HEADERS` and `imagepayload.cc` to `SOURCES`;
- `bglresource.hh` to `HEADERS` and `bglresource.cc` to `SOURCES`.

Without these the application links undefined `ImagePayload::tiffToBmp` and
`BglResource` symbols even while the new suite passes.

## Test strategy

A new no-display suite, `tests/tst_resource_payload`.

- `QTEST_GUILESS_MAIN` (no `QApplication`, no display).
- Compiled with `QMAKE_CXXFLAGS += -D_GLIBCXX_ASSERTIONS`, matching the Arch package,
  so the pre-repair empty accesses abort exactly as the reported coredump did.
- Suite-local `Config` substitutes (as `tst_btree_index` does).
- A minimal concrete `Dictionary::Class` fixture implementing **all six** pure
  virtuals with their real signatures: `getName`, `getProperties`,
  `getArticleCount`, `getWordCount`, `prefixMatch`, and `getArticle`; the first four
  return inert values and the two request methods return instant empty requests.
- `dictionary.hh` is listed in `HEADERS` so its `Q_OBJECT` classes are MOCed.
- Link closure: `dictionary.cc`, `file.cc`, `fsencoding.cc`, `mutex.cc`, `utf8.cc`,
  `wstring.cc`, `wstring_qt.cc`, `wildcard.cc`, `folding.cc`, `gddebug.cc`,
  `zipfile.cc`, `splitfile.cc`, `imagepayload.cc`, `bglresource.cc`, `tiff.cc` (with
  `!CONFIG(no_extra_tiff_handler): LIBS += -ltiff`, exactly as `tst_utf8_wstring`
  does), the suite source; Qt core and gui; `-lz`.

Cases:

| # | Case | Class |
| --- | --- | --- |
| 1 | `payloadAssign_emptyLeavesVectorEmpty` | failing-first |
| 2 | `payloadAssign_copiesExactBytes` | control |
| 3 | `payloadAssign_replacesLongerContent` | control |
| 4 | `payloadAppend_emptyIsNoOp` | failing-first |
| 5 | `payloadAppend_appendsExactly` | control |
| 6 | `loadFromFile_zeroByteFileIsEmpty` | failing-first (the reported abort; also the SoundDir path) |
| 7 | `loadFromFile_readsExactBytes` | control |
| 8 | `setIsolatedCssPayload_commentOnlyYieldsEmpty` | failing-first |
| 9 | `setIsolatedCssPayload_pageOnlyYieldsEmpty` | failing-first |
| 10 | `setIsolatedCssPayload_prefixesOrdinaryRules` | control |
| 11 | `tiffToBmp_emptyInputYieldsEmpty` | control |
| 12 | `tiffToBmp_undecodableInputYieldsEmpty` | control |
| 13 | `tiffToBmp_producesBmpFromValidTiff` | control |
| 14 | `gdTiffDecodesCommittedOneBitFixture` | control |
| 15 | `bglReadDirectoryEntry_emptyNameStillConsumesOffset` | failing-first |
| 16 | `bglDecompress_validEmptyStreamSucceedsEmpty` | failing-first |
| 17 | `bglDecompress_corruptStreamFails` | control |
| 18 | `bglDecompress_exactNonEmptyRoundTrip` | control |
| 19 | `bglDecompress_advertisedLengthMismatchFails` | control |
| 20 | `bglDecompress_emptyCompressedInputFails` | failing-first |

The failing-first cases run individually in the extraction phase; the controls must
pass in both phases.

**TIFF fixture.** Cases 13 and 14 use a committed one-bit, single-sample, 2x2 TIFF
under `tests/fixtures/imagepayload/tiny-1bit-no-photometric.tiff` that deliberately
omits the `PhotometricInterpretation` tag: Qt's TIFF reader rejects it while libtiff
still accepts and reads it, which forces the bundled decoder. The two scanlines are
`0x40` and `0x80`; `GdTiff` maps palette index 0 to white and index 1 to black
(`tiff.cc:129-132`), so the expected pixels are (0,0) white, (1,0) black, (0,1)
black, (1,1) white. Case 13 first asserts
`QVERIFY( ImagePayload::decodeWithQt( bytes ).isNull() )` — if a future Qt accepts the
fixture, the test fails loudly instead of silently skipping the fallback — then
asserts that `tiffToBmp` returns a 2x2 BMP with exactly that grid. Case 14 calls
`ImagePayload::decodeWithGdTiff` directly with the same oracle. Removing the GdTiff
invocation from `tiffToBmp` therefore fails case 13.

### Failing-first extraction and concrete mutations

The seams first land as behaviour-preserving extractions: `payload.hh` keeps the
current unsafe `resize`+`front()` body, `setIsolatedCssPayload` keeps the current
copy, `tiffToBmp` moves the decode sequence, the BGL bodies move verbatim (including
their unsafe `front()` formation), and the backends are rewired. The suite then
aborts in cases 1, 4, 6, 8, 9, 15, 16 and 20 with the same `__glibcxx_assert_fail`
signature as the coredump. Only then are the guards added. The BGL extraction is not
an exception to this rule: it arrives with the unsafe bodies and its cases are
failing-first, so no control is required to pass before a guard exists.

The audit mutation-probes exactly these mechanisms, each with its killing case:

| Mechanism | Mutation | Observable failure |
| --- | --- | --- |
| `Payload::assign` | remove the zero-size guard | case 1 aborts |
| `Payload::append` | remove the zero-size guard | case 4 aborts |
| `File::loadFromFile` | remove the empty-vector guard | case 6 aborts |
| `setIsolatedCssPayload` | substitute the old `resize`+`front()` copy | cases 8 and 9 abort |
| `BglResource::readDirectoryEntry` | skip the offset when the name is empty | case 15 reads the next record wrongly |
| `BglResource::decompress` | reject a zero-byte output | case 16 fails |
| `BglResource::decompress` | drop the advertised-length comparison | case 19 fails |
| `BglResource::decompress` | drop the zero-input guard | case 20 aborts |

`tiffToBmp` takes a pointer and a size, so it has no removable guard; cases 11–14 pin
its empty, undecodable, positive and fallback-decoder semantics, and removing its
GdTiff invocation fails case 13 through the no-photometric fixture. Caller wiring is
covered by the inventory check below.

### Suite inventory

`tests/tests.pro` pins the count in three places — the `count(GD_TEST_NAMES, 20)`
predicate and its error text, the `message("tests/tests.pro: exact 20 suites: ...")`
line, and the header comment — and `goldendict.pro:772` repeats "all 20 suites" in the
`tests-build` comment. All four change to 21, and `tst_resource_payload` is inserted
between `tst_regex` and `tst_save_article` in the sorted required list. Historical
spec and plan documents keep their recorded counts.

### Wiring coverage

All emptying and replacing logic lives in six linked, tested seams: `Payload::assign`,
`Payload::append`, `File::loadFromFile`, `Class::setIsolatedCssPayload`,
`ImagePayload::tiffToBmp`, and `BglResource`. Each backend row above becomes a single
call to one of them; SoundDir's hand-rolled load disappears entirely.

The audit verifies wiring deterministically:

- it checks the diff touches exactly the inventory rows;
- it enumerates every remaining `front()` and `&…front()` in the resource translation
  units (`mdx`, `gls`, `stardict`, `slob`, `zim`, `website`, `dsl`, `xdxf`, `bgl`,
  `epwing`, `sounddir`, `file`) and requires each remaining one to be provably inside
  a non-empty branch (`mdx.cc:887` is guarded by `data.size() > sizeof(pattern)`),
  so an empty-TIFF `QImage::fromData(&data.front(), …)` cannot hide behind a repaired
  BMP copy;
- it mutation-probes the six mechanisms above and records the killing case.

## Risks

- **Assertion-only abort.** The undefined access is only observable with
  `-D_GLIBCXX_ASSERTIONS`; the suite enables it deliberately, other suites do not.
- **Unlinked backend wiring.** Backend one-line calls cannot be executed from the
  suite; the seam extraction plus the audit's inventory and mutation probes are the
  compensating control.
- **Conditional translation units.** `zim.cc` compiles only under
  `CONFIG+=zim_support`, and EPWING is included unless `no_epwing_support` is set.
  Verification therefore builds with the package's feature set.
- **Feature drift.** `MAKE_EXTRA_TIFF_HANDLER` is on by default; the suite and the
  application build both link `tiff.cc`/`-ltiff` under the same condition.
- **TIFF fallback coverage.** A host with Qt's TIFF plugin can satisfy the positive
  case without GdTiff; case 14 and the composition-line check cover that gap
  explicitly instead of claiming a mutation that cannot fail.
- **Behaviour drift.** The empty outcome table above is the contract; the
  specification pins each row to its source line.

## Verification

- Build and run the full 21-suite gate from the integration worktree.
- Build the application with the package's configuration —
  `qmake6 goldendict.pro PREFIX=/usr CONFIG+=chinese_conversion_support
  CONFIG+=zim_support` — and require compile evidence that `zim.cc`, `epwing.cc`,
  `tiff.cc`, `imagepayload.cc`, `bglresource.cc`, and every touched backend produced
  objects; the default build alone may compile an empty `zim.cc`.
- Record the failing-first aborts for cases 1, 4, 6, 8, 9, 15, 16 and 20 on the extraction.
- Run the front() inventory and every mutation probe in the table above during the audit.
- If the dictionary folder that produced the coredump is identified, load that
  dictionary's real empty local resource through the repaired tree before delivery.

## Acceptance criteria

- The reported abort no longer occurs on a zero-byte local resource file.
- No resource-payload site in the inventory retains the unguarded
  resize/copy/read or decode-pointer idiom.
- `tst_resource_payload` aborts on the pre-repair extraction and passes on the
  repaired code; the 21-suite gate and the feature-enabled application build pass.
- Non-empty payloads and the success-versus-missing contract are unchanged.

## Review dispositions

Round 1:

- DR-1 accepted: diagnosis corrected to `File::loadFromFile`; the CSS copy is a
  separate defect.
- DR-2 accepted: inventory expanded to every resource-payload site across backends;
  article producers excluded with explicit invariants.
- DR-3 accepted: the `QBuffer::constData` error disappears with `tiffToBmp`.
- DR-4 accepted: the exact empty outcome and `dataSize()` value are stated above.
- DR-5 accepted: every live count pin changes, including `goldendict.pro`.
- DR-6 accepted: the new suite is assertion-enabled so the RED abort is real.
- DR-7 accepted: suite-local stubs, a minimal fixture, MOC registration, and
  `QTEST_GUILESS_MAIN` are specified.
- DR-8 accepted: risky logic is extracted into linked seams; wiring is verified by
  the audit's inventory check and mutation probes.
- DR-9 accepted: the XDXF article copy is excluded as unreachable.

Round 2:

- DR2-1 accepted: `tiff.cc` and conditional `-ltiff` join the suite closure.
- DR2-2 accepted: the fixture implements all six pure virtuals.
- DR2-3 accepted: backend-specific empty outcomes replace the generic rows; BGL
  keeps a valid empty stream successful.
- DR2-4 accepted: `BglResource::readDirectoryEntry` consumes the offset and case 15
  pins it.
- DR2-5 accepted: the inventory check enumerates remaining `front()` uses, and
  SoundDir/BGL become linked seams.
- DR2-6 accepted: each mechanism has a concrete mutation and killing case.
- DR2-7 accepted: verification builds with the package feature set.
- DR2-8 accepted: SLOB/ZIM fallback and archive rows are corrected.
- DR2-9 accepted: the offsets are `run+0x342` and `run+0x348`.

Round 3:

- DR3-1 accepted: the image and archive rows are backend-specific and now include
  SoundDir, EPWING, ZIP-backed resources, and undecodable non-empty images.
- DR3-2 accepted: BGL record/decompression logic is extracted and tested; SoundDir
  now calls the tested `File::loadFromFile`.
- DR3-3 accepted: the new translation units are registered in `goldendict.pro`.
- DR3-4 accepted: the BGL row is scoped to fully readable records; truncated index
  I/O is explicitly unchanged and out of scope.
- DR3-5 accepted: a named one-bit fixture, a direct GdTiff case, and an honest
  composition-line check replace the unprovable mutation claim.
- DR3-6 accepted: the SoundDir API and SLOB non-empty invariant are corrected.

Round 4:

- DR4-1 accepted: the BGL extraction keeps its unsafe bodies, so cases 15, 16 and 20
  are failing-first and the controls pass only after their guards land.
- DR4-2 accepted: `decompress` takes the advertised size explicitly, keeps the exact
  length comparison, and cases 18–20 plus the length-comparison mutation pin it.
- DR4-3 accepted: the fixture omits `PhotometricInterpretation` so Qt rejects it and
  libtiff accepts it; case 13 asserts the rejection and the pixel grid, and removing
  the fallback fails it.
- DR4-4 accepted: `readDirectoryEntry` is `void`; read exceptions propagate and no
  false outcome is claimed.
- DR4-5 accepted: the XDXF empty-ZIP row now attributes the result to
  `IndexedZip::loadFile`, not to a seam the path does not call.

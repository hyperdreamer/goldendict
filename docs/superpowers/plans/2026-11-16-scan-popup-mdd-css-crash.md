# Scan Popup MDD CSS Crash Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use the deterministic subagent-driven-development controller to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Stop empty dictionary resource payloads from aborting the scan popup while preserving non-empty bytes, lookup order, and each backend's successful-empty versus missing result.

**Architecture:** Extract the approved payload, stylesheet, image, and BGL seams and wire every specified resource caller before changing the unsafe zero-length bodies. Record eight individual assertion aborts and twelve passing controls, then add the guards and verify the seams, caller wiring, feature variants, aggregate gate, and application build.

**Tech Stack:** Qt 6/qmake, C++17, QtTest with `QTEST_GUILESS_MAIN`, libstdc++ `-D_GLIBCXX_ASSERTIONS`, zlib `-lz`, and conditional libtiff `-ltiff` under `!CONFIG(no_extra_tiff_handler)`.

**Authority:** `docs/superpowers/specs/2026-11-16-scan-popup-mdd-css-crash-spec.md` at `c93871f8`, and `docs/superpowers/specs/2026-11-16-scan-popup-mdd-css-crash-design.md`. The specification's corrected offsets, live source locations, six fixture virtuals, BGL controls, and feature flags take precedence over earlier design wording. The specification records implementation base `89cc501c9fbbb1ef25b9d580060083ca23a01aab`; the planning checkout is `c93871f8`.

**File structure and ownership:** `payload.hh` owns byte replacement and append; `dictionary.hh/.cc` own CSS isolation-to-payload; `imagepayload.hh/.cc` own Qt/GdTiff decoding and BMP composition; `bglresource.hh/.cc` own directory records and decompression. Existing resource functions only delegate at the specified construction points. `file.cc` owns the guarded file read; `sounddir.cc` delegates to it; `epwing.cc` delegates its cache copy. `goldendict.pro` registers the modules; `tests/tst_resource_payload/tst_resource_payload.pro` and `tst_resource_payload.cc` provide the assertion-enabled closure; `tests/fixtures/imagepayload/tiny-1bit-no-photometric.tiff` supplies the pinned fallback oracle; `tests/tests.pro` owns the exact aggregate inventory.

## Global Constraints

- Use Qt 6/qmake and C++17. The new suite includes `QMAKE_CXXFLAGS += -D_GLIBCXX_ASSERTIONS`, `LIBS += -lz`, and `!CONFIG(no_extra_tiff_handler): LIBS += -ltiff` exactly. `dictionary.hh` is MOCed. Both the application and the suite list `tiff.cc` unconditionally; define `MAKE_EXTRA_TIFF_HANDLER` and link `-ltiff` only under `!CONFIG(no_extra_tiff_handler)`.
- The suite is a QCoreApplication suite with `QTEST_GUILESS_MAIN`; it runs without a display even though `tests/testsupport.pri` links Widgets and WebEngine. Do not change shared project feature defaults or introduce dependencies.
- Execute this plan in numeric order in one serial `resource-payload` lane: Tasks 1–7 extract/register/wire, Task 8 records RED and controls, Task 9 adds guards, Tasks 10–12 verify. Do not repair a failing-first access before Task 8 records it. Extraction preserves unsafe zero-length `front()` formation in `Payload::assign`, `Payload::append`, `File::loadFromFile`, the stylesheet copy, and the BGL name/decompression bodies.
- The eight failing-first cases are 1, 4, 6, 8, 9, 15, 16, and 20. Each must independently abort with libstdc++'s empty-vector assertion and exit status 134 under `_GLIBCXX_ASSERTIONS`; another reason for `SIGABRT` is insufficient. Controls 2, 3, 5, 7, 10–14, and 17–19 pass in both phases. All 20 pass after guards.
- `Dictionary::DataRequest::dataSize()` is `data.size()` when `hasAnyData` is true and `-1` otherwise. Helpers never publish success. Preserve existing catches, mutex scopes, lookup precedence, cancellation checks, and flag assignment points, except the specified BGL success condition.
- Preserve backend outcomes: MDD's existing zero-byte local file stays `-1`; non-empty MDD CSS transformed to empty stays `0`. GLS/StarDict/SLOB/ZIM transformed-empty CSS and successful website empty CSS stay `0`. DSL/GLS/StarDict/XDXF loaded empty resources stay `0`; their undecodable non-empty images retain raw bytes. SLOB/ZIM initially empty resource strings stay rejected with `-1`; their non-empty undecodable images stay successful with empty payload (`0`). SoundDir existing empty files and EPWING successfully opened empty cache files stay `0`; missing/unopened resources stay `-1`. XDXF's empty stored ZIP entries, including TIFF names, stay successful (`0`) through `IndexedZip::loadFile` and its existing success flag. SLOB/ZIM empty archive entries stay rejected before copying. Fully readable BGL valid zero-output streams succeed (`0`); empty compressed storage, corrupt streams, and advertised-length mismatches stay unsuccessful (`-1`). BGL short index reads keep propagating `File::Class::read` exceptions; no new completed-outcome guarantee is added.
- For BGL retain `for (size_t count = resourcesCount; count--;)` and its per-record `isCancelled.loadAcquire()` check; call `findResource(idx, 1, nameLowercased, offset)` in each iteration. Retain the initial cancellation check, request-name bytewise `tolower`, outer `idxMutex`, `idx.seek(resourceListOffset)`, resource-block `idx.seek(offset)`, existing `dataMutex`, warning on decompression failure, first-match break even on decode failure, and final `finish()`.
- Final byte contracts: assignment replaces exactly `size` bytes and clears a seed for zero size; zero-size append is a no-op; non-empty copies preserve embedded zeros. Sources are separate from destinations. CSS parsing/output and `isolateCSS()` protection remain unchanged. Image conversion uses the original `data` in DSL/GLS/StarDict/XDXF and the proven non-empty `resource` string in SLOB/ZIM; assign only non-empty BMP bytes using `QByteArray::constData()`, never `QBuffer::constData()`.
- Update every live count pin to 21. The final ordered inventory is `tst_article_bridge tst_article_history tst_article_load tst_article_schemes tst_article_stream tst_btree_index tst_encoding tst_external_content tst_hotkeys tst_htmlescape tst_langcoder tst_linkrewrite tst_platformwindow tst_portal_hotkeys tst_print_pipeline tst_regex tst_resource_payload tst_save_article tst_utf8_wstring tst_waylandselection tst_website_frames`. Preserve closed discovery, missing/extra errors, `CHECK_MAKEFILE = Makefile.tests`, aggregate generation at `tests/Makefile.tests`, parallel `tests-build`, serial `tests-run`, root `check` depending on `tests-run`, and the wrapper around both build and run. Do not edit historical inventory statements.
- Application verification uses a fresh shadow build with `qmake6 "$source_root/goldendict.pro" PREFIX=/usr CONFIG+=chinese_conversion_support CONFIG+=zim_support`, leaving EPWING and TIFF enabled. Require fresh, non-empty objects for `file sounddir epwing mdx gls stardict slob zim website xdxf dsl bgl imagepayload bglresource tiff`, plus compile-command evidence of `MAKE_ZIM_SUPPORT` for `zim.cc`, absence of `NO_EPWING_SUPPORT` for `epwing.cc`, and `MAKE_EXTRA_TIFF_HANDLER` for `tiff.cc`.
- Scope exclusions: no code edit to generic dispatch in `article_netmgr.cc:95`, BGL article copy at `bgl.cc:933-935`, XDXF article copy at `xdxf.cc:657`, website fixed-wrapper article append at `website.cc:203`, DSL article-image probe guarded at `dsl.cc:987`, XDXF article chunk reader at `xdxf.cc:689`, or storage primitive at `chunkedstorage.cc:179`. Preserve MDD redirection's `data.size() > sizeof(pattern)` guard at `mdx.cc:887`. Line numbers refer to the planning base; locate the named functions after earlier edits.
- The only committed fixture added is `tests/fixtures/imagepayload/tiny-1bit-no-photometric.tiff`, with the exact specification bytes. BGL directory and zlib fixtures stay in the suite source. Do not invent test slot names, substitute fixtures, skip Qt's TIFF rejection, add format-level stubs, or link whole dictionary backends into the pure suite.
- Capture command output, exit status, source revision, fixture/configuration identity, and mutation/restoration results in controller-owned role evidence. Do not add new repository evidence filenames. Every task has a commit step; evidence-only tasks use an empty commit after attaching their results to the controller's report. Do not amend the pinned plan or tick its checkboxes during execution.
- Hard safety: never execute `reboot`, `shutdown`, `poweroff`, `halt`, `kexec`, any `systemctl` power/sleep verb, `init 0|6`, or `telinit 0|6`; never restart/stop the display manager or `graphical.target`; never terminate the session or signal the PM's process tree. No host transitions.
- If a host transition is required, write `reboot-required.json` with `schema: 1`, the observed `trigger`, `evidence: [{path, sha256}]` calculated from actual evidence files, and `provenance: {run_id, lane, attempt, dispatch_key, observed_boot_id}`; report it and stop. Supplied provenance is run_id `pm-run-20261116-203148-068023b6`, lane `null`, attempt `1`, dispatch_key `pm:pm-run-20261116-203148-068023b6:role-planner:lane-null:task-null:epoch-1:attempt-1`. Read `observed_boot_id` from `/proc/sys/kernel/random/boot_id`; at planning time it is `4f378ad8-925f-497b-b110-bd19c7ed7d70`.

## Task 1: Extract byte operations and delegate file and cache payloads

**Implementer tier:** Advanced

**Lane:** resource-payload

**Category:** file-loader

**Files:**

- Create: `payload.hh`.
- Inspect, preserve until Task 9: `file.cc:55-65`, `file.hh`'s `loadFromFile` declaration.
- Modify: `sounddir.cc:372-384`, `epwing.cc:1-40,780-788`.
- Eventual tests: `tests/tst_resource_payload/tst_resource_payload.cc`, cases 1–7.

**Interfaces:**

- Consumes: `void File::loadFromFile(std::string const & n, std::vector<char> & data)` and `FsEncoding::encode(fileName)` from existing code.
- Produces: header-only `void Payload::assign(std::vector<char> & destination, char const * bytes, std::size_t size)` and `void Payload::append(std::vector<char> & destination, char const * bytes, std::size_t size)`.
- Produces: SoundDir delegation to the unchanged loader and EPWING delegation to `Payload::assign`, without changing request outcomes.

- [ ] Add `payload.hh` with an include guard and only `<cstddef>`, `<cstring>`, `<vector>`. First extract these intentionally unsafe bodies; the zero-length repairs belong to Task 9:

```cpp
namespace Payload {
inline void assign( std::vector< char > & destination,
                    char const * bytes, std::size_t size )
{
  destination.resize( size );
  std::memcpy( &destination.front(), bytes, size );
}

inline void append( std::vector< char > & destination,
                    char const * bytes, std::size_t size )
{
  std::size_t const oldSize = destination.size();
  destination.resize( oldSize + size );
  std::memcpy( &destination.front() + oldSize, bytes, size );
}
}
```

- [ ] In `SoundDirDictionary::getResource`, keep construction of `DataRequestInstant(true)` and its `getData()` reference; replace the hand-rolled `File::Class` open/size/rewind/read with `File::loadFromFile( FsEncoding::encode( fileName ), data );`. Keep the surrounding `try`, return, and `catch (File::Ex&)` returning `DataRequestInstant(false)`. Leave `File::loadFromFile`'s existing unsafe read unchanged for the reproducer.
- [ ] Include `payload.hh` in `epwing.cc`; replace only the cache-copy resize/memcpy with `Payload::assign( data, buffer.constData(), buffer.size() );`. Keep `dataMutex`, the successful-open branch, and its following `hasAnyData = true`.
- [ ] Run `git diff --check` and inspect the two complete resource functions. No regression suite exists yet; do not invent a temporary suite. Once Task 6 exists, Task 8 executes `./tst_resource_payload payloadAssign_copiesExactBytes payloadAssign_replacesLongerContent payloadAppend_appendsExactly loadFromFile_readsExactBytes` in the assertion-enabled shadow build: controls 2, 3, 5, 7 must pass. Cases 1 `payloadAssign_emptyLeavesVectorEmpty`, 4 `payloadAppend_emptyIsNoOp`, and 6 `loadFromFile_zeroByteFileIsEmpty` must still abort individually before guards, then turn green in Task 9.
- [ ] Commit:

```sh
git add payload.hh sounddir.cc epwing.cc
git commit -m "refactor: extract resource byte copies and delegate resource file loads"
```

## Task 2: Extract CSS payload isolation and wire stylesheet callers

**Implementer tier:** Advanced

**Lane:** resource-payload

**Category:** stylesheet

**Files:**

- Modify: `dictionary.hh:280-300`, `dictionary.cc:1-40,278-425`.
- Modify: `mdx.cc:937-940`, `gls.cc:1459-1462`, `stardict.cc:1753-1756`, `slob.cc:1672-1680`, `zim.cc:1588-1595`, and the relevant include sections.
- Modify: `website.cc:1-30,352-359`.
- Eventual tests: `tests/tst_resource_payload/tst_resource_payload.cc`, cases 8–10; website append is also pinned by cases 4–5 plus the wiring audit.

**Interfaces:**

- Consumes: protected `void Dictionary::Class::isolateCSS(QString & css, QString const & wrapperSelector = QString())`.
- Consumes: `void Payload::append(std::vector<char> & destination, char const * bytes, std::size_t size)`.
- Produces: public `void Dictionary::Class::setIsolatedCssPayload(QString & css, std::vector<char> & data, QString const & wrapperSelector = QString())`.

- [ ] Add the exact public declaration, leaving `isolateCSS()` protected. Add this extraction implementation in namespace `Dictionary`, adjacent to `Class::isolateCSS`; it deliberately retains the old empty-result copy until Task 9:

```cpp
void Class::setIsolatedCssPayload( QString & css, std::vector< char > & data,
                                   QString const & wrapperSelector )
{
  isolateCSS( css, wrapperSelector );
  QByteArray bytes = css.toUtf8();
  data.resize( bytes.size() );
  memcpy( &data.front(), bytes.data(), bytes.size() );
}
```

- [ ] Replace each resource-only isolation/copy sequence with its exact call:

| Resource function | Replacement |
| --- | --- |
| `MddResourceRequest::run()` | `dict.setIsolatedCssPayload( css, data, ".mdict" );` |
| `GlsResourceRequest::run()` | `dict.setIsolatedCssPayload( css, data );` |
| `StardictResourceRequest::run()` | `dict.setIsolatedCssPayload( css, data );` |
| `SlobResourceRequest::run()` | `dict.setIsolatedCssPayload( css, data, ".slobdict" );` |
| `ZimResourceRequest::run()` | `dict.setIsolatedCssPayload( css, data, ".zimdict" );` |

- [ ] Preserve StarDict's preceding URL rewrite, MDD's earlier success gate, SLOB/ZIM's preceding empty-resource rejection, every existing mutex, and each backend's later success assignment. In `WebSiteResourceRequest::requestFinished`, include `payload.hh` and replace only `prevSize`/resize/memcpy with `Payload::append( data, cssData.constData(), cssData.size() );` under the existing mutex. Retain `isolateWebCSS`, redirects/errors, and the following success publication.
- [ ] Run `git diff --check` and audit the six resource functions against the table. In Task 8's shadow build, run `./tst_resource_payload setIsolatedCssPayload_prefixesOrdinaryRules`: control 10 passes with exact CSS `#gdfrom-resource-payload-test .entry { color: red; }`. Cases 8 `setIsolatedCssPayload_commentOnlyYieldsEmpty` and 9 `setIsolatedCssPayload_pageOnlyYieldsEmpty` still abort separately before Task 9. Case 4 still aborts in `Payload::append`; case 5 passes. These cases become green through Task 9's shared guards.
- [ ] Commit:

```sh
git add dictionary.hh dictionary.cc mdx.cc gls.cc stardict.cc slob.cc zim.cc website.cc
git commit -m "refactor: share stylesheet resource payload construction"
```

## Task 3: Extract image decoding and wire all six TIFF resource branches

**Implementer tier:** Advanced

**Lane:** resource-payload

**Category:** image

**Files:**

- Create: `imagepayload.hh`, `imagepayload.cc`.
- Modify: `dsl.cc:2034-2062`, `gls.cc:1384-1414`, `stardict.cc:1678-1708`, `xdxf.cc:1132-1163`, `slob.cc:1686-1718`, `zim.cc:1601-1633`, and their relevant includes.
- Inspect, preserve: `tiff.hh`, `tiff.cc`, `indexedzip.cc:78-81`.
- Eventual tests: `tests/tst_resource_payload/tst_resource_payload.cc`, controls 11–14.

**Interfaces:**

- Consumes: `void Payload::assign(std::vector<char> & destination, char const * bytes, std::size_t size)` and `bool GdTiff::tiffToQImage(char const * data, int size, QImage & image)`.
- Produces, namespace `ImagePayload`: `QImage decodeWithQt(char const * bytes, std::size_t size)`, `QImage decodeWithGdTiff(char const * bytes, std::size_t size)`, and `QByteArray tiffToBmp(char const * bytes, std::size_t size)`.

- [ ] Add an include-guarded `imagepayload.hh` containing `<cstddef>`, `<QByteArray>`, `<QImage>`, and the three exact declarations. Add `imagepayload.cc` with `imagepayload.hh`, `tiff.hh`, `<QBuffer>`, and this composition:

```cpp
namespace ImagePayload {
QImage decodeWithQt( char const * bytes, std::size_t size )
{
  if ( !size )
    return QImage();
  return QImage::fromData( reinterpret_cast< uchar const * >( bytes ), size );
}

QImage decodeWithGdTiff( char const * bytes, std::size_t size )
{
  if ( !size )
    return QImage();
#ifdef MAKE_EXTRA_TIFF_HANDLER
  QImage image;
  GdTiff::tiffToQImage( bytes, size, image );
  return image;
#else
  (void) bytes;
  return QImage();
#endif
}

QByteArray tiffToBmp( char const * bytes, std::size_t size )
{
  if ( !size )
    return QByteArray();
  QImage image = decodeWithQt( bytes, size );
  if ( image.isNull() )
    image = decodeWithGdTiff( bytes, size );
  if ( image.isNull() )
    return QByteArray();
  QBuffer buffer;
  if ( !buffer.open( QIODevice::WriteOnly ) || !image.save( &buffer, "BMP" ) )
    return QByteArray();
  return buffer.data();
}
}
```

- [ ] Include the two seams in each backend. Replace only the TIFF resource decode/BMP-copy block, keeping the existing filename conditions and lock scopes. DSL, GLS, StarDict, XDXF use:

```cpp
QByteArray const bmp = ImagePayload::tiffToBmp( data.data(), data.size() );
if ( !bmp.isEmpty() )
{
  // Retain this caller's existing payload-assignment dataMutex scope here.
  Payload::assign( data, bmp.constData(), bmp.size() );
}
```

SLOB/ZIM use `ImagePayload::tiffToBmp( resource.data(), resource.size() )` instead. Keep their existing earlier `resource.empty()` rejection. Do not assign an empty conversion, and do not form `&data.front()` for either decoder. Preserve raw undecodable sources in DSL/GLS/StarDict/XDXF, and the empty destination on SLOB/ZIM decode failure.
- [ ] Replace SLOB/ZIM's non-image defensive archive copies with `Payload::assign( data, resource.data(), resource.size() );`, retaining their source rejection and success publication. Do not edit the excluded DSL article-image probe or XDXF article copy/chunk reader.
- [ ] Run `git diff --check` and inspect all six branches. Once the suite is added, Task 8 runs `./tst_resource_payload tiffToBmp_emptyInputYieldsEmpty tiffToBmp_undecodableInputYieldsEmpty tiffToBmp_producesBmpFromValidTiff gdTiffDecodesCommittedOneBitFixture`: controls 11–14 already pass before guards. In enabled configuration, 13 must first prove Qt rejects the committed TIFF, then prove the fallback BMP's exact grid; 14 checks GdTiff directly. No image case is classified as an assertion RED case; zero-size pointer/length input does not itself imply vector `front()` access.
- [ ] Commit:

```sh
git add imagepayload.hh imagepayload.cc dsl.cc gls.cc stardict.cc xdxf.cc slob.cc zim.cc
git commit -m "refactor: share size-aware TIFF resource conversion"
```

## Task 4: Extract BGL record lookup and resource decompression

**Implementer tier:** Advanced

**Lane:** resource-payload

**Category:** bgl

**Files:**

- Create: `bglresource.hh`, `bglresource.cc`.
- Modify: `bgl.cc:1-50,1022-1084`.
- Eventual tests: `tests/tst_resource_payload/tst_resource_payload.cc`, cases 15–20.

**Interfaces:**

- Consumes: `File::Class::read<T>()`, `File::Class::read(void *, size_t)`, `File::Class::seek`, existing BGL cancellation and mutex behavior, and zlib `uncompress`.
- Produces these exact declarations in an include-guarded header with `"file.hh"`, `<cstddef>`, `<stdint.h>`, `<string>`, `<vector>`:

```cpp
namespace BglResource {
struct DirectoryEntry
{
  std::string name;
  uint32_t offset;
};
void readDirectoryEntry( File::Class & idx, DirectoryEntry & entry );
bool findResource( File::Class & idx, uint32_t count,
                   std::string const & lowercaseName, uint32_t & offset );
bool decompress( std::vector< char > const & compressed,
                 std::size_t advertisedSize, std::vector< char > & data );
}
```

- [ ] In `bglresource.cc`, include `"bglresource.hh"`, `<cctype>`, `<zlib.h>`. Extract the unsafe read/decompression expressions, preserving exact-length validation that already exists in BGL:

```cpp
namespace BglResource {
void readDirectoryEntry( File::Class & idx, DirectoryEntry & entry )
{
  std::vector< char > nameData( idx.read< uint32_t >() );
  idx.read( &nameData.front(), nameData.size() );
  entry.name.assign( &nameData.front(), nameData.size() );
  entry.offset = idx.read< uint32_t >();
}

bool findResource( File::Class & idx, uint32_t count,
                   std::string const & lowercaseName, uint32_t & offset )
{
  while ( count-- )
  {
    DirectoryEntry entry;
    readDirectoryEntry( idx, entry );
    if ( entry.name.empty() )
      continue;
    for ( std::string::iterator i = entry.name.begin(); i != entry.name.end(); ++i )
      *i = std::tolower( *i );
    if ( entry.name == lowercaseName )
    {
      offset = entry.offset;
      return true;
    }
  }
  return false;
}

bool decompress( std::vector< char > const & compressed,
                 std::size_t advertisedSize, std::vector< char > & data )
{
  data.resize( advertisedSize );
  uLongf actualLength = data.size();
  return uncompress( reinterpret_cast< Bytef * >( &data.front() ), &actualLength,
                     reinterpret_cast< Bytef const * >( &compressed.front() ),
                     compressed.size() ) == Z_OK && actualLength == advertisedSize;
}
}
```

- [ ] Include `bglresource.hh` in `bgl.cc`. Retain the outer loop and its cancellation point. Replace directory parsing/matching with this call-site structure; keep the existing initial cancellation, lowercasing, `idxMutex`, `idx.seek(resourceListOffset)`, and final `finish()` surrounding it:

```cpp
for ( size_t count = resourcesCount; count--; )
{
  if ( isCancelled.loadAcquire() )
    break;
  uint32_t offset;
  if ( !BglResource::findResource( idx, 1, nameLowercased, offset ) )
    continue;
  idx.seek( offset );
  Mutex::Lock _( dataMutex );
  uint32_t advertisedSize = idx.read< uint32_t >();
  vector< char > compressedData( idx.read< uint32_t >() );
  if ( !compressedData.empty() )
    idx.read( &compressedData.front(), compressedData.size() );
  if ( !BglResource::decompress( compressedData, advertisedSize, data ) )
    gdWarning( "Failed to decompress resource \"%s\", ignoring it.\n", name.c_str() );
  else
    hasAnyData = true;
  break;
}
```

Do not catch index I/O exceptions. Changing the stored compressed vector to `vector<char>` matches the approved seam; its guarded file read does not repair the seam's intentionally unsafe empty compressed `front()` yet.
- [ ] Run `git diff --check` and inspect the complete request function. In Task 8's extraction build, `./tst_resource_payload bglDecompress_corruptStreamFails bglDecompress_exactNonEmptyRoundTrip bglDecompress_advertisedLengthMismatchFails` passes controls 17–19. Cases 15 `bglFindResource_emptyNameSkippedAfterOffset`, 16 `bglDecompress_validEmptyStreamSucceedsEmpty`, and 20 `bglDecompress_emptyCompressedInputFails` must still abort individually; all turn green in Task 9. Case 17 uses advertised size 1, case 19 checks sizes 2 and 4, and case 20 checks size 1 before 0. Do not remove the existing exact-length check to manufacture RED.
- [ ] Commit:

```sh
git add bglresource.hh bglresource.cc bgl.cc
git commit -m "refactor: extract BGL resource lookup and decompression seams"
```

## Task 5: Register the new production modules in the application

**Implementer tier:** Standard

**Lane:** resource-payload

**Category:** build

**Files:**

- Modify: `goldendict.pro:288-407,444-557`.
- Inspect: `goldendict.pro:610-639`, `tiff.cc:4-154`.

**Interfaces:**

- Consumes: header-only `Payload::assign/append(std::vector<char> &, char const *, std::size_t)`; `ImagePayload::decodeWithQt/decodeWithGdTiff(char const *, std::size_t) -> QImage`, `ImagePayload::tiffToBmp(char const *, std::size_t) -> QByteArray`; `BglResource::readDirectoryEntry(File::Class &, DirectoryEntry &)`, `findResource(File::Class &, uint32_t, std::string const &, uint32_t &) -> bool`, and `decompress(std::vector<char> const &, std::size_t, std::vector<char> &) -> bool`.
- Produces: application compilation/link registration for `imagepayload.cc`, `bglresource.cc`, and their three new headers.

- [ ] Add `payload.hh` beside `file.hh`, `imagepayload.hh` beside `tiff.hh`, and `bglresource.hh` beside `bgl.hh` in `HEADERS`. Add `imagepayload.cc` beside `tiff.cc` and `bglresource.cc` beside `bgl.cc` in `SOURCES`.
- [ ] Retain unconditional `tiff.cc` registration and existing conditional `MAKE_EXTRA_TIFF_HANDLER`/`-ltiff`. Keep EPWING and ZIM feature conditions unchanged; suite-only registration cannot satisfy application acceptance.
- [ ] Run `git diff --check` and `rg -n 'payload[.]hh|imagepayload[.](hh|cc)|bglresource[.](hh|cc)|tiff[.]cc' goldendict.pro`. Require each new registration exactly once. This task introduces no new case and repairs no RED case; it makes the production implementation used by cases 1–20 linkable. Cases 1, 4, 6, 8, 9, 15, 16, 20 remain intentionally unsafe until Task 9.
- [ ] Reserve the exact fresh application build from Task 12 for acceptance after guards; do not run the aggregate early and mistake its expected assertion abort for an integration defect. Task 12 verifies all named backend/module objects and the enabled feature macros.
- [ ] Commit:

```sh
git add goldendict.pro
git commit -m "build: register shared resource payload modules"
```

## Task 6: Add the complete pure regression suite and pinned fixtures

**Implementer tier:** Advanced

**Lane:** resource-payload

**Category:** tests

**Files:**

- Create: `tests/tst_resource_payload/tst_resource_payload.pro`, `tests/tst_resource_payload/tst_resource_payload.cc`.
- Create: `tests/fixtures/imagepayload/tiny-1bit-no-photometric.tiff`.
- Inspect: `tests/testsupport.pri`, `tests/support/fixtures.hh`, `tests/tst_btree_index/tst_btree_index.cc`'s Config stubs, `dictionary.hh`'s six pure virtuals.

**Interfaces:**

- Consumes: `Payload::assign/append(std::vector<char> &, char const *, std::size_t)`; `File::loadFromFile(std::string const &, std::vector<char> &)`; public `Dictionary::Class::setIsolatedCssPayload(QString &, std::vector<char> &, QString const & = QString())`; `ImagePayload::decodeWithQt/decodeWithGdTiff(char const *, std::size_t) -> QImage`, `tiffToBmp(char const *, std::size_t) -> QByteArray`; `BglResource::findResource(File::Class &, uint32_t, std::string const &, uint32_t &) -> bool`, `decompress(std::vector<char> const &, std::size_t, std::vector<char> &) -> bool`.
- Consumes: `TestSupport::repositoryRoot() -> QString`, `FsEncoding::encode(QString const &) -> std::string`, and the exact dictionary fixture signatures below.
- Produces: `ResourcePayloadDictionary`, `ResourcePayloadTests`, exactly the 20 approved test slots, and a 124-byte committed TIFF oracle. The eight RED slots are implemented now but executed individually in Task 8.

- [ ] Create the complete qmake file verbatim:

```qmake
TARGET = tst_resource_payload
include(../testsupport.pri)
QT += core gui
QMAKE_CXXFLAGS += -D_GLIBCXX_ASSERTIONS
SOURCES += $$GD_REPO_ROOT/dictionary.cc \
           $$GD_REPO_ROOT/file.cc \
           $$GD_REPO_ROOT/fsencoding.cc \
           $$GD_REPO_ROOT/mutex.cc \
           $$GD_REPO_ROOT/utf8.cc \
           $$GD_REPO_ROOT/wstring.cc \
           $$GD_REPO_ROOT/wstring_qt.cc \
           $$GD_REPO_ROOT/wildcard.cc \
           $$GD_REPO_ROOT/folding.cc \
           $$GD_REPO_ROOT/gddebug.cc \
           $$GD_REPO_ROOT/zipfile.cc \
           $$GD_REPO_ROOT/splitfile.cc \
           $$GD_REPO_ROOT/imagepayload.cc \
           $$GD_REPO_ROOT/bglresource.cc \
           $$GD_REPO_ROOT/tiff.cc \
           tst_resource_payload.cc
HEADERS += $$GD_REPO_ROOT/dictionary.hh \
           $$GD_REPO_ROOT/payload.hh \
           $$GD_REPO_ROOT/imagepayload.hh \
           $$GD_REPO_ROOT/bglresource.hh
LIBS += -lz
!CONFIG(no_extra_tiff_handler): LIBS += -ltiff
```

- [ ] Create the test source with the following includes and fixtures. The concrete dictionary implements all six pure virtuals with the specification's complete signatures; do not expose `isolateCSS()`:

```cpp
#include <QtTest>
#include <QFile>
#include <QTemporaryDir>
#include <QColor>
#include <vector>
#include "config.hh"
#include "dictionary.hh"
#include "file.hh"
#include "fsencoding.hh"
#include "payload.hh"
#include "imagepayload.hh"
#include "bglresource.hh"
#include "fixtures.hh"

namespace Config {
bool isPortableVersion() throw() { return false; }
QString getPortableVersionDictionaryDir() throw() { return QString(); }
}

class ResourcePayloadDictionary: public Dictionary::Class
{
public:
  ResourcePayloadDictionary():
    Dictionary::Class( "resource-payload-test", std::vector< std::string >() ) {}
  std::string getName() throw() override { return "resource-payload-test"; }
  std::map< Dictionary::Property, std::string > getProperties() throw() override
  { return {}; }
  unsigned long getArticleCount() throw() override { return 0; }
  unsigned long getWordCount() throw() override { return 0; }
  sptr< Dictionary::WordSearchRequest > prefixMatch(
    gd::wstring const &, unsigned long ) THROW_SPEC( std::exception ) override
  { return new Dictionary::WordSearchRequestInstant; }
  sptr< Dictionary::DataRequest > getArticle(
    gd::wstring const &, std::vector< gd::wstring > const &,
    gd::wstring const &, bool ) THROW_SPEC( std::exception ) override
  { return new Dictionary::DataRequestInstant( true ); }
};

class ResourcePayloadTests: public QObject
{
  Q_OBJECT
private slots:
  void payloadAssign_emptyLeavesVectorEmpty()
  {
    std::vector< char > data = { 'o', 'l', 'd' };
    Payload::assign( data, nullptr, 0 );
    QVERIFY( data.empty() );
  }
  void payloadAssign_copiesExactBytes()
  {
    char const bytes[] = { char(0x41), char(0x00), char(0xff) };
    std::vector< char > data;
    Payload::assign( data, bytes, sizeof(bytes) );
    QCOMPARE( data.size(), std::size_t(3) );
    QVERIFY( data == std::vector< char >( bytes, bytes + sizeof(bytes) ) );
  }
  void payloadAssign_replacesLongerContent()
  {
    std::vector< char > data = { 'o', 'l', 'd' };
    char const bytes[] = { char(0x78), char(0x79) };
    Payload::assign( data, bytes, sizeof(bytes) );
    QCOMPARE( data.size(), std::size_t(2) );
    QVERIFY( data == std::vector< char >( bytes, bytes + sizeof(bytes) ) );
  }
  void payloadAppend_emptyIsNoOp()
  {
    std::vector< char > data;
    Payload::append( data, nullptr, 0 );
    QVERIFY( data.empty() );
  }
  void payloadAppend_appendsExactly()
  {
    std::vector< char > data = { char(0x61) };
    char const bytes[] = { char(0x62), char(0x63) };
    Payload::append( data, bytes, sizeof(bytes) );
    QCOMPARE( data.size(), std::size_t(3) );
    QCOMPARE( QByteArray(data.data(), int(data.size())), QByteArray::fromHex("616263") );
  }
  void loadFromFile_zeroByteFileIsEmpty()
  {
    QTemporaryDir dir;
    QVERIFY( dir.isValid() );
    QFile file( dir.filePath("empty") );
    QVERIFY( file.open(QIODevice::WriteOnly) );
    file.close();
    std::vector< char > data;
    File::loadFromFile( FsEncoding::encode(file.fileName()), data );
    QVERIFY( data.empty() );
  }
  void loadFromFile_readsExactBytes()
  {
    QTemporaryDir dir;
    QVERIFY( dir.isValid() );
    QFile file( dir.filePath("bytes") );
    QByteArray const bytes = QByteArray::fromHex("00ff3100");
    QVERIFY( file.open(QIODevice::WriteOnly) );
    QCOMPARE( file.write(bytes), qint64(bytes.size()) );
    file.close();
    std::vector< char > data;
    File::loadFromFile( FsEncoding::encode(file.fileName()), data );
    QCOMPARE( data.size(), std::size_t(4) );
    QCOMPARE( QByteArray(data.data(), int(data.size())), bytes );
  }
  void setIsolatedCssPayload_commentOnlyYieldsEmpty()
  {
    ResourcePayloadDictionary dict;
    QString css = "/* only */";
    std::vector< char > data = { char(0x6f), char(0x6c), char(0x64) };
    dict.setIsolatedCssPayload( css, data );
    QVERIFY( css.isEmpty() );
    QVERIFY( data.empty() );
  }
  void setIsolatedCssPayload_pageOnlyYieldsEmpty()
  {
    ResourcePayloadDictionary dict;
    QString css = "@page { size: A4; }";
    std::vector< char > data = { char(0x6f), char(0x6c), char(0x64) };
    dict.setIsolatedCssPayload( css, data );
    QVERIFY( css.isEmpty() );
    QVERIFY( data.empty() );
  }
  void setIsolatedCssPayload_prefixesOrdinaryRules()
  {
    ResourcePayloadDictionary dict;
    QString css = ".entry { color: red; }";
    std::vector< char > data;
    dict.setIsolatedCssPayload( css, data );
    QString const expected = "#gdfrom-resource-payload-test .entry { color: red; }";
    QCOMPARE( css, expected );
    QCOMPARE( data.size(), std::size_t(expected.toUtf8().size()) );
    QCOMPARE( QByteArray(data.data(), int(data.size())), expected.toUtf8() );
  }
  void tiffToBmp_emptyInputYieldsEmpty()
  {
    QVERIFY( ImagePayload::decodeWithQt(nullptr, 0).isNull() );
    QVERIFY( ImagePayload::decodeWithGdTiff(nullptr, 0).isNull() );
    QVERIFY( ImagePayload::tiffToBmp(nullptr, 0).isEmpty() );
  }
  void tiffToBmp_undecodableInputYieldsEmpty()
  {
    QByteArray const bytes = QByteArray::fromHex("0001");
    QVERIFY( ImagePayload::decodeWithQt(bytes.constData(), bytes.size()).isNull() );
    QVERIFY( ImagePayload::tiffToBmp(bytes.constData(), bytes.size()).isEmpty() );
  }
  void tiffToBmp_producesBmpFromValidTiff()
  {
    QFile file( TestSupport::repositoryRoot()
                + "/tests/fixtures/imagepayload/tiny-1bit-no-photometric.tiff" );
    QVERIFY( file.open(QIODevice::ReadOnly) );
    QByteArray const bytes = file.readAll();
    QCOMPARE( bytes.size(), qsizetype(124) );
    QVERIFY( ImagePayload::decodeWithQt(bytes.constData(), bytes.size()).isNull() );
    QByteArray const bmp = ImagePayload::tiffToBmp(bytes.constData(), bytes.size());
#ifdef MAKE_EXTRA_TIFF_HANDLER
    QVERIFY( !bmp.isEmpty() );
    QImage image = QImage::fromData(
      reinterpret_cast<uchar const *>(bmp.constData()), bmp.size(), "BMP" );
    QVERIFY( !image.isNull() );
    QCOMPARE( image.width(), 2 );
    QCOMPARE( image.height(), 2 );
    QCOMPARE( image.pixelColor(0, 0), QColor(Qt::white) );
    QCOMPARE( image.pixelColor(1, 0), QColor(Qt::black) );
    QCOMPARE( image.pixelColor(0, 1), QColor(Qt::black) );
    QCOMPARE( image.pixelColor(1, 1), QColor(Qt::white) );
#else
    QVERIFY( bmp.isEmpty() );
#endif
  }
  void gdTiffDecodesCommittedOneBitFixture()
  {
    QFile file( TestSupport::repositoryRoot()
                + "/tests/fixtures/imagepayload/tiny-1bit-no-photometric.tiff" );
    QVERIFY( file.open(QIODevice::ReadOnly) );
    QByteArray const bytes = file.readAll();
    QCOMPARE( bytes.size(), qsizetype(124) );
    QImage image = ImagePayload::decodeWithGdTiff(bytes.constData(), bytes.size());
#ifdef MAKE_EXTRA_TIFF_HANDLER
    QVERIFY( !image.isNull() );
    QCOMPARE( image.width(), 2 );
    QCOMPARE( image.height(), 2 );
    QCOMPARE( image.pixelColor(0, 0), QColor(Qt::white) );
    QCOMPARE( image.pixelColor(1, 0), QColor(Qt::black) );
    QCOMPARE( image.pixelColor(0, 1), QColor(Qt::black) );
    QCOMPARE( image.pixelColor(1, 1), QColor(Qt::white) );
#else
    QVERIFY( image.isNull() );
#endif
  }
  void bglFindResource_emptyNameSkippedAfterOffset()
  {
    QByteArray const bytes = QByteArray::fromHex(
      "000000000100000009000000696d6167652e74696688776655" );
    QCOMPARE( bytes.size(), qsizetype(25) );
    QTemporaryDir dir;
    QVERIFY( dir.isValid() );
    QFile file( dir.filePath("directory") );
    QVERIFY( file.open(QIODevice::WriteOnly) );
    QCOMPARE( file.write(bytes), qint64(bytes.size()) );
    file.close();
    File::Class idx( FsEncoding::encode(file.fileName()), "rb" );
    uint32_t offset = 0;
    idx.seek(0);
    QVERIFY( !BglResource::findResource(idx, 2, std::string(), offset) );
    QCOMPARE( offset, uint32_t(0) );
    idx.seek(0);
    QVERIFY( BglResource::findResource(idx, 2, "image.tif", offset) );
    QCOMPARE( offset, uint32_t(0x55667788) );
  }
  void bglDecompress_validEmptyStreamSucceedsEmpty()
  {
    QByteArray const bytes = QByteArray::fromHex("789c030000000001");
    std::vector< char > compressed(bytes.constData(), bytes.constData() + bytes.size());
    std::vector< char > data;
    QVERIFY( BglResource::decompress(compressed, 0, data) );
    QVERIFY( data.empty() );
  }
  void bglDecompress_corruptStreamFails()
  {
    QByteArray const bytes = QByteArray::fromHex("789c030000000000");
    std::vector< char > compressed(bytes.constData(), bytes.constData() + bytes.size());
    std::vector< char > data;
    QVERIFY( !BglResource::decompress(compressed, 1, data) );
  }
  void bglDecompress_exactNonEmptyRoundTrip()
  {
    QByteArray const bytes = QByteArray::fromHex("789c4b4c4a0600024d0127");
    std::vector< char > compressed(bytes.constData(), bytes.constData() + bytes.size());
    std::vector< char > data;
    QVERIFY( BglResource::decompress(compressed, 3, data) );
    QCOMPARE( data.size(), std::size_t(3) );
    QCOMPARE( QByteArray(data.data(), int(data.size())), QByteArray::fromHex("616263") );
  }
  void bglDecompress_advertisedLengthMismatchFails()
  {
    QByteArray const bytes = QByteArray::fromHex("789c4b4c4a0600024d0127");
    std::vector< char > compressed(bytes.constData(), bytes.constData() + bytes.size());
    std::vector< char > data;
    QVERIFY( !BglResource::decompress(compressed, 2, data) );
    QVERIFY( !BglResource::decompress(compressed, 4, data) );
  }
  void bglDecompress_emptyCompressedInputFails()
  {
    std::vector< char > const compressed;
    std::vector< char > data;
    QVERIFY( !BglResource::decompress(compressed, 1, data) );
    QVERIFY( !BglResource::decompress(compressed, 0, data) );
  }
};

QTEST_GUILESS_MAIN( ResourcePayloadTests )
#include "tst_resource_payload.moc"
```

- [ ] Create the binary TIFF from the exact specification bytes using this command from the implementation root. It intentionally omits tag 262, has nine IFD entries and strip offset 122, and stores scanlines `40 80`:

```sh
python3 - <<'PY'
from pathlib import Path
payload = bytes.fromhex('''
49 49 2a 00 08 00 00 00 09 00 00 01 04 00 01 00
00 00 02 00 00 00 01 01 04 00 01 00 00 00 02 00
00 00 02 01 03 00 01 00 00 00 01 00 00 00 03 01
03 00 01 00 00 00 01 00 00 00 11 01 04 00 01 00
00 00 7a 00 00 00 15 01 03 00 01 00 00 00 01 00
00 00 16 01 04 00 01 00 00 00 02 00 00 00 17 01
04 00 01 00 00 00 02 00 00 00 1c 01 03 00 01 00
00 00 01 00 00 00 00 00 00 00 40 80
''')
assert len(payload) == 124
path = Path('tests/fixtures/imagepayload/tiny-1bit-no-photometric.tiff')
path.parent.mkdir(parents=True, exist_ok=True)
path.write_bytes(payload)
assert path.read_bytes() == payload
PY
```

The pixel oracle is row 0 `[white, black]`, row 1 `[black, white]`. The directory fixture is exactly `00 00 00 00 01 00 00 00 09 00 00 00 69 6d 61 67 65 2e 74 69 66 88 77 66 55`; its first entry's offset is consumed before skipping its empty name. The valid empty, corrupt-checksum, and `abc` streams are exactly those in the source above. Empty compressed storage is zero bytes, not a zero-output stream.
- [ ] Run `git diff --check`. Build and execute the twelve extraction controls only, leaving the eight abort probes for Task 8's individual evidence collection:

```sh
set -e
source_root=$(git rev-parse --show-toplevel)
test_build=$(mktemp -d /tmp/gd-resource-payload-test.XXXXXX)
(
  cd "$test_build"
  qmake6 "$source_root/tests/tst_resource_payload/tst_resource_payload.pro"
  make -j"$(nproc)"
  ./tst_resource_payload payloadAssign_copiesExactBytes \
    payloadAssign_replacesLongerContent payloadAppend_appendsExactly \
    loadFromFile_readsExactBytes setIsolatedCssPayload_prefixesOrdinaryRules \
    tiffToBmp_emptyInputYieldsEmpty tiffToBmp_undecodableInputYieldsEmpty \
    tiffToBmp_producesBmpFromValidTiff gdTiffDecodesCommittedOneBitFixture \
    bglDecompress_corruptStreamFails bglDecompress_exactNonEmptyRoundTrip \
    bglDecompress_advertisedLengthMismatchFails
)
```

Expected: cases 2, 3, 5, 7, 10–14, 17–19 pass with the real production closure and TIFF fallback. Cases 1, 4, 6, 8, 9, 15, 16, 20 remain RED. Do not run the whole suite to infer eight failures from a single abort.
- [ ] Commit:

```sh
git add tests/tst_resource_payload/tst_resource_payload.pro tests/tst_resource_payload/tst_resource_payload.cc tests/fixtures/imagepayload/tiny-1bit-no-photometric.tiff
git commit -m "test: add assertion-enabled resource payload regression fixtures"
```

## Task 7: Update the live aggregate inventory to exactly 21 suites

**Implementer tier:** Standard

**Lane:** resource-payload

**Category:** build

**Files:**

- Modify: `tests/tests.pro:9,123-146`.
- Modify: `goldendict.pro:769-774`'s `tests-build` comment.
- Inspect: the existing root test recipes at `goldendict.pro:753-784`.

**Interfaces:**

- Consumes: `tests/tst_resource_payload/tst_resource_payload.pro` and existing closed discovery of `tests/tst_*/tst_*.pro`.
- Produces: exact 21-suite `GD_REQUIRED_SUITE_NAMES`, `!count(GD_TEST_NAMES, 21)`, updated error/status text, and unchanged root gate targets.

- [ ] Change the aggregate header to “exactly the 21 required suites”; use this complete ordered name list:

```qmake
GD_REQUIRED_SUITE_NAMES = \
    tst_article_bridge tst_article_history tst_article_load \
    tst_article_schemes tst_article_stream tst_btree_index tst_encoding \
    tst_external_content tst_hotkeys tst_htmlescape tst_langcoder \
    tst_linkrewrite tst_platformwindow \
    tst_portal_hotkeys tst_print_pipeline tst_regex tst_resource_payload tst_save_article \
    tst_utf8_wstring tst_waylandselection tst_website_frames
```

- [ ] Set `!count(GD_TEST_NAMES, 21): error("Expected exactly 21 required suites; found: $$GD_TEST_NAMES")` and `message("tests/tests.pro: exact 21 suites: $$GD_TEST_NAMES")`. Change “all 20 suites” to “all 21 suites” in the application's `tests-build` comment. Preserve every recipe and wrapper; do not generate an aggregate in the repository root.
- [ ] Run `git diff --check` and `qmake6 tests/tests.pro -o tests/Makefile.tests`. Expected: `tests/tests.pro: exact 21 suites` with the required sorted list; no missing/extra-project error. Configuration is the local test for this build-only task. It registers all cases 1–20 but does not change their semantics: eight RED cases remain until Task 9. Run the full root gate only in Task 12.
- [ ] Commit:

```sh
git add tests/tests.pro goldendict.pro
git commit -m "build: enforce the exact 21-suite regression inventory"
```

## Task 8: Record all eight extraction aborts and twelve passing controls

**Implementer tier:** Standard

**Lane:** resource-payload

**Category:** tests

**Files:**

- Execute, do not modify: `tests/tst_resource_payload/tst_resource_payload.pro`, `tests/tst_resource_payload/tst_resource_payload.cc` and its committed TIFF fixture.
- Inspect, preserve during evidence capture: `payload.hh`, `file.cc`, `dictionary.cc`, `bglresource.cc`.
- Evidence: controller-owned task report; no new repository evidence file.

**Interfaces:**

- Consumes: the assertion-enabled `tst_resource_payload` QtTest executable, exposing the exact twenty slot names in the following commands.
- Produces: one recorded extraction revision, eight individually identified empty-vector assertion exits, and a passing twelve-control run. This is the prerequisite for guard changes.

- [ ] Record `git rev-parse HEAD` and inspect compile commands for `-D_GLIBCXX_ASSERTIONS`. Configure a fresh extraction build and run the specification's block from the implementation root:

```sh
set -e
source_root=$(git rev-parse --show-toplevel)
extraction_build=$(mktemp -d /tmp/gd-resource-payload-extraction.XXXXXX)
(
  cd "$extraction_build"
  qmake6 "$source_root/tests/tst_resource_payload/tst_resource_payload.pro"
  make -j"$(nproc)"
  ulimit -c 0
  set +e
  for slot in payloadAssign_emptyLeavesVectorEmpty payloadAppend_emptyIsNoOp \
              loadFromFile_zeroByteFileIsEmpty \
              setIsolatedCssPayload_commentOnlyYieldsEmpty \
              setIsolatedCssPayload_pageOnlyYieldsEmpty \
              bglFindResource_emptyNameSkippedAfterOffset \
              bglDecompress_validEmptyStreamSucceedsEmpty \
              bglDecompress_emptyCompressedInputFails; do
    ./tst_resource_payload "$slot"
    status=$?
    test "$status" -eq 134 || exit 1
  done
  set -e
  ./tst_resource_payload payloadAssign_copiesExactBytes \
    payloadAssign_replacesLongerContent payloadAppend_appendsExactly \
    loadFromFile_readsExactBytes setIsolatedCssPayload_prefixesOrdinaryRules \
    tiffToBmp_emptyInputYieldsEmpty tiffToBmp_undecodableInputYieldsEmpty \
    tiffToBmp_producesBmpFromValidTiff gdTiffDecodesCommittedOneBitFixture \
    bglDecompress_corruptStreamFails bglDecompress_exactNonEmptyRoundTrip \
    bglDecompress_advertisedLengthMismatchFails
)
```

- [ ] Attach per-slot output and exit status, not only the loop's status. Require `!this->empty()`/`std::vector<char>::front()` libstdc++ assertion evidence and status 134 for each of cases 1, 4, 6, 8, 9, 15, 16, 20. Confirm controls 2, 3, 5, 7, 10–14, 17–19 pass. A compile failure, missing fixture, other signal, or failing control blocks the guard phase; correct extraction/fixture mistakes without adding guards, then repeat the affected evidence.
- [ ] Note that case 15 aborts on the empty name, 16 on the empty output address, and 20's first advertised-size-1 call on the empty compressed address. Keep case 17's non-empty destination and case 19's existing length comparison intact. This task records RED and controls; it turns no failing-first case green.
- [ ] Commit the evidence checkpoint after submitting the report:

```sh
git commit --allow-empty -m "test: record eight resource payload extraction aborts and passing controls"
```

## Task 9: Add the zero-length guards and exact BGL outcomes

**Implementer tier:** Advanced

**Lane:** resource-payload

**Category:** module

**Files:**

- Modify: `payload.hh`, `file.cc:55-65`, `dictionary.cc`'s `Class::setIsolatedCssPayload`, `bglresource.cc`'s `readDirectoryEntry` and `decompress`.
- Inspect, preserve correct wiring: `imagepayload.cc`, `bgl.cc`, `sounddir.cc`, `epwing.cc`, the five CSS resource functions, six TIFF branches, and website append.
- Execute unchanged: `tests/tst_resource_payload/tst_resource_payload.cc`.

**Interfaces:**

- Consumes and finalizes: `Payload::assign/append(std::vector<char> &, char const *, std::size_t)`; `File::loadFromFile(std::string const &, std::vector<char> &)`; `Dictionary::Class::setIsolatedCssPayload(QString &, std::vector<char> &, QString const & = QString())`.
- Consumes and finalizes: `BglResource::DirectoryEntry { std::string name; uint32_t offset; }`, `readDirectoryEntry(File::Class &, DirectoryEntry &) -> void`, `findResource(File::Class &, uint32_t, std::string const &, uint32_t &) -> bool`, `decompress(std::vector<char> const &, std::size_t, std::vector<char> &) -> bool`.
- Preserves: `ImagePayload::decodeWithQt/decodeWithGdTiff(char const *, std::size_t) -> QImage` and `tiffToBmp(char const *, std::size_t) -> QByteArray` from the already size-aware extraction.
- Produces: all final empty-safety contracts without changing request success ownership.

- [ ] Confirm Task 8's eight assertion records and twelve passing controls exist. Replace only the extracted byte operation bodies:

```cpp
inline void assign( std::vector< char > & destination,
                    char const * bytes, std::size_t size )
{
  destination.resize( size );
  if ( size )
    std::memcpy( &destination.front(), bytes, size );
}

inline void append( std::vector< char > & destination,
                    char const * bytes, std::size_t size )
{
  if ( !size )
    return;
  std::size_t const oldSize = destination.size();
  destination.resize( oldSize + size );
  std::memcpy( &destination.front() + oldSize, bytes, size );
}
```

- [ ] In `File::loadFromFile`, retain open, seek, resize, rewind; wrap its read in `if ( !data.empty() )`. Do not change `file.hh`'s signature or catches. Include `payload.hh` in `dictionary.cc` and replace the stylesheet helper with:

```cpp
void Class::setIsolatedCssPayload( QString & css, std::vector< char > & data,
                                   QString const & wrapperSelector )
{
  isolateCSS( css, wrapperSelector );
  QByteArray const bytes = css.toUtf8();
  Payload::assign( data, bytes.constData(), bytes.size() );
}
```

- [ ] Replace BGL's unsafe temporary name-vector read with the exact directory rule. Keep `findResource`'s loop, offset consumption before empty-name skip, first-match behavior, unchanged output offset on no match, and bytewise `tolower`:

```cpp
void readDirectoryEntry( File::Class & idx, DirectoryEntry & entry )
{
  entry.name.resize( idx.read< uint32_t >() );
  if ( !entry.name.empty() )
    idx.read( &entry.name.front(), entry.name.size() );
  entry.offset = idx.read< uint32_t >();
}
```

- [ ] Replace decompression with the scratch-backed valid-empty rule. Resize first, reject empty compressed input before element access, require `Z_OK` and exact produced length, and retain the same behavior for corrupt/short/oversized output:

```cpp
bool decompress( std::vector< char > const & compressed,
                 std::size_t advertisedSize, std::vector< char > & data )
{
  data.resize( advertisedSize );
  if ( compressed.empty() )
    return false;
  Bytef scratch;
  uLongf actualLength = advertisedSize ? data.size() : 1;
  Bytef * destination = advertisedSize
    ? reinterpret_cast< Bytef * >( &data.front() ) : &scratch;
  int const result = uncompress(
    destination, &actualLength,
    reinterpret_cast< Bytef const * >( &compressed.front() ), compressed.size() );
  return result == Z_OK && actualLength == advertisedSize;
}
```

Do not add `hasAnyData` to helpers or an exception handler around index reads. The BGL caller already guards compressed file reads and publishes success only on true. Verify the image seam still short-circuits empty input and all six callers assign only non-empty BMP; no new image guard mutation or caller semantics is needed.
- [ ] Run `git diff --check`, then a fresh enabled shadow build from the implementation root:

```sh
set -e
source_root=$(git rev-parse --show-toplevel)
test_build=$(mktemp -d /tmp/gd-resource-payload-test.XXXXXX)
(
  cd "$test_build"
  qmake6 "$source_root/tests/tst_resource_payload/tst_resource_payload.pro"
  make -j"$(nproc)"
  ./tst_resource_payload
)
```

Expected: all twenty named cases pass (plus QtTest lifecycle entries); formerly RED cases 1, 4, 6, 8, 9, 15, 16, 20 now pass and twelve controls remain green. Do not report QtTest's lifecycle count as twenty-two specification cases. Save the repaired revision and successful run for the mutation baselines.
- [ ] Commit:

```sh
git add payload.hh file.cc dictionary.cc bglresource.cc
git commit -m "fix: guard empty resource payload copies reads and BGL decoding"
```

## Task 10: Prove the eight specified mutations with restored controls

**Implementer tier:** Advanced

**Lane:** resource-payload

**Category:** verification

**Files:**

- Read only in the implementation checkout: `payload.hh`, `file.cc`, `dictionary.cc`, `bglresource.cc`, `imagepayload.cc`, the new test project/source, and committed TIFF fixture.
- Mutate only corresponding files in a disposable copy of the repaired tree; no additional repository file.
- Evidence: controller-owned mutation report.

**Interfaces:**

- Consumes: repaired `Payload::assign/append(std::vector<char> &, char const *, std::size_t)`, `File::loadFromFile(std::string const &, std::vector<char> &)`, `Dictionary::Class::setIsolatedCssPayload(QString &, std::vector<char> &, QString const & = QString())`, `BglResource::readDirectoryEntry(File::Class &, DirectoryEntry &)`, `findResource(File::Class &, uint32_t, std::string const &, uint32_t &) -> bool`, `decompress(std::vector<char> const &, std::size_t, std::vector<char> &) -> bool`, and `ImagePayload::tiffToBmp(char const *, std::size_t) -> QByteArray`.
- Produces: eight isolated passing-baseline → named failing mutant → restored-green records; source bytes in the implementation checkout remain unchanged.

- [ ] Create a disposable source copy from the committed repaired revision and a fresh shadow build. Do not copy generated Makefiles/objects that could compile the original sources:

```sh
set -e
source_root=$(git rev-parse --show-toplevel)
probe_root=$(mktemp -d /tmp/gd-resource-payload-mutations.XXXXXX)
git archive HEAD | tar -x -C "$probe_root"
test_build=$(mktemp -d /tmp/gd-resource-payload-test.XXXXXX)
(
  cd "$test_build"
  qmake6 "$probe_root/tests/tst_resource_payload/tst_resource_payload.pro"
  make -j"$(nproc)"
  ./tst_resource_payload
)
```

Expected: all twenty cases pass; fixture lookup resolves to the disposable source copy. Inspect generated compile paths to confirm mutation sources are used. Keep this shell's `probe_root` and `test_build` values in the role evidence.
- [ ] For each following row, first run its named slot green, apply only that row's source mutation, rebuild with `make -C "$test_build" -j"$(nproc)"`, and execute the exact slot command from the table. Use `ulimit -c 0` and temporarily disable shell fail-fast only around expected failures, recording status and diagnostic. An assertion mutation requires 134 and the empty-vector text; other rows require the identified QtTest assertion failure, not a build error.

| Mutation, one at a time | Exact killer command (from the disposable build directory) | Required observation |
| --- | --- | --- |
| Remove `Payload::assign`'s nonzero guard, keep `front()` copy | `./tst_resource_payload payloadAssign_emptyLeavesVectorEmpty` | Case 1 aborts. |
| Remove `Payload::append`'s zero-size early return, keep `front()` append address | `./tst_resource_payload payloadAppend_emptyIsNoOp` | Case 4 aborts. |
| Remove `File::loadFromFile`'s `!data.empty()` read guard | `./tst_resource_payload loadFromFile_zeroByteFileIsEmpty` | Case 6 aborts. |
| Replace CSS helper's `Payload::assign` with the old resize/`&data.front()` memcpy | `./tst_resource_payload setIsolatedCssPayload_commentOnlyYieldsEmpty` and separately `./tst_resource_payload setIsolatedCssPayload_pageOnlyYieldsEmpty` | Cases 8 and 9 each abort. |
| Return from `readDirectoryEntry` for empty name before reading its offset, so `findResource` continues without consuming the offset | `./tst_resource_payload bglFindResource_emptyNameSkippedAfterOffset` | Case 15's later `image.tif` lookup fails. |
| Reject advertised size zero in `BglResource::decompress` | `./tst_resource_payload bglDecompress_validEmptyStreamSucceedsEmpty` | Case 16 fails its success assertion. |
| Remove `actualLength == advertisedSize`, retain `result == Z_OK` | `./tst_resource_payload bglDecompress_advertisedLengthMismatchFails` | Case 19's advertised-size-4 call incorrectly succeeds. |
| Remove only the GdTiff fallback invocation from `tiffToBmp` | `./tst_resource_payload tiffToBmp_producesBmpFromValidTiff` | Case 13 first proves Qt rejection, then fails its non-empty BMP requirement. |

- [ ] After each mutant, restore the changed file's exact bytes from the implementation checkout, rebuild from the disposable source path, and rerun every killer for that row green before changing another mechanism. Record the restored source identity and command outputs. In the skipped-offset mutant, leave the default-initialized string empty and avoid using the uninitialized offset for an empty record; the intended failure is bounded misalignment of the second record, not unrelated undefined behavior.
- [ ] Confirm case 20 is covered by the complete baseline/restored suite and the earlier failing-first run; it is not an additional guard-removal mutation. Do not claim removing an `ImagePayload` empty-input check must abort. Run `./tst_resource_payload` once on the final restored disposable tree, and `git status --short` in the implementation checkout to verify isolation.
- [ ] Commit the evidence checkpoint after attaching all eight results:

```sh
git commit --allow-empty -m "test: record resource payload mutation and restoration evidence"
```

## Task 11: Audit every production resource function and reject wiring regressions

**Implementer tier:** Advanced

**Lane:** resource-payload

**Category:** verification

**Files:**

- Read only: `file.cc`, `sounddir.cc`, `epwing.cc`, `mdx.cc`, `gls.cc`, `stardict.cc`, `slob.cc`, `zim.cc`, `website.cc`, `dsl.cc`, `xdxf.cc`, `bgl.cc`, `dictionary.cc`, `payload.hh`, `imagepayload.cc`, `bglresource.cc`.
- Inspect exclusions without editing: `article_netmgr.cc`, `indexedzip.cc`, `chunkedstorage.cc`, the excluded article functions.
- Negative controls: disposable copies of `sounddir.cc` and `xdxf.cc` only; no added repository audit script.

**Interfaces:**

- Consumes: `File::loadFromFile(std::string const &, std::vector<char> &)`, `Payload::assign/append(std::vector<char> &, char const *, std::size_t)`, `Dictionary::Class::setIsolatedCssPayload(QString &, std::vector<char> &, QString const & = QString())`, `ImagePayload::tiffToBmp(char const *, std::size_t) -> QByteArray`, `BglResource::findResource(File::Class &, uint32_t, std::string const &, uint32_t &) -> bool`, and `decompress(std::vector<char> const &, std::size_t, std::vector<char> &) -> bool`.
- Produces: a function-scoped audit of every production-table row, an enumeration of remaining pointer formations with their guards/exclusions, and three rejected negative controls with restored passing checks.

- [ ] Read each complete function below and record the specified seam at its payload construction point. Use this coverage matrix; passing linked unit tests cannot stand in for these checks:

| Function/site | Required wiring and invariant |
| --- | --- |
| `File::loadFromFile` | Only reads through `front()` inside `!data.empty()`. |
| `SoundDirDictionary::getResource` | Delegates to `File::loadFromFile(FsEncoding::encode(fileName), data)`; instant success and missing-file catch remain. |
| `EpwingResourceRequest::run` | `Payload::assign(data, buffer.constData(), buffer.size())` inside successful-open/dataMutex scope, then existing success publication. |
| `MddResourceRequest::run` | CSS helper with `.mdict`; original loader/precedence/flag gate retained; redirection pointer guarded by `data.size() > sizeof(pattern)`. |
| `GlsResourceRequest::run` | CSS helper without wrapper; image seam uses original `data`; only non-empty BMP replaces it. |
| `StardictResourceRequest::run` | URL rewrite before CSS helper; image seam uses original `data`, assigns only non-empty BMP. |
| `SlobResourceRequest::run` | Empty `resource` rejected first; `.slobdict` CSS helper; image seam uses `resource`, not destination; archive copy uses `Payload::assign`; success assignment retained. |
| `ZimResourceRequest::run` | Empty `resource` rejected first; `.zimdict` CSS helper; image seam uses `resource`; defensive archive assignment and existing success retained. |
| `WebSiteResourceRequest::requestFinished` | `Payload::append(data, cssData.constData(), cssData.size())` under existing mutex, then success; fixed-wrapper article append excluded. |
| `DslResourceRequest::run` | Image seam uses original `data`; non-empty BMP-only replacement; raw undecodable source and success preserved. |
| `XdxfResourceRequest::run` | Both empty stored ZIP TIFF and non-empty TIFF enter `tiffToBmp(data.data(), data.size())`; never `QImage::fromData(&data.front(), ...)`; empty BMP is not assigned; existing success remains. |
| `BglResourceRequest::run` | One-record `findResource` inside retained cancellation loop; guarded compressed byte read; correct size-field order; decompress success gates `hasAnyData`; first-match break, locks, seek, warning, finish preserved. |

- [ ] Use these read-only searches to locate every remaining pointer/copy/decode formation, then classify matches inside the full named functions. A global `memcpy` count is insufficient:

```sh
rg -n 'front\(|memcpy|QImage::fromData|GdTiff::tiffToQImage|Payload::|ImagePayload::|BglResource::|setIsolatedCssPayload|hasAnyData|resource[.]empty|loadFromFile' mdx.cc gls.cc stardict.cc slob.cc zim.cc website.cc dsl.cc xdxf.cc bgl.cc epwing.cc sounddir.cc file.cc
rg -n 'isCancelled|resourcesCount|resourceListOffset|idx[.]seek|advertisedSize|compressedData|hasAnyData|finish\(' bgl.cc
```

Require each remaining resource-payload address formation to have an explicit non-empty proof. Name all excluded article/storage matches using the global exclusions; do not broaden the repair. Inspect `indexedzip.cc`'s empty stored entry branch to attribute XDXF zero-size success correctly. Cases 1–20 remain green; this task independently establishes backend reachability of their seams.
- [ ] Use this inline, read-only function-scoped check for the two sites whose negative controls are mandatory. For the repaired baseline first set `audit_root=$(git rev-parse --show-toplevel)`; for each negative control set `audit_root` to that disposable source directory before rerunning the same check. This supplements the complete human/source matrix above and leaves SoundDir's unrelated `chunk.front()` index-record proof outside the payload check:

```sh
AUDIT_ROOT="$audit_root" python3 - <<'PY'
import os
import re
from pathlib import Path
root = Path(os.environ['AUDIT_ROOT'])

def function(path, signature):
    source = (root / path).read_text()
    start = source.index(signature)
    # Resource functions end at an unindented closing brace in this codebase.
    end = source.index('\n}', start) + 2
    return re.sub(r'\s+', '', source[start:end])

sound = function('sounddir.cc', 'SoundDirDictionary::getResource(')
assert 'File::loadFromFile(FsEncoding::encode(fileName),data)' in sound
assert '&data.front()' not in sound
assert 'DataRequestInstant(true)' in sound
assert 'catch(File::Ex&)' in sound and 'DataRequestInstant(false)' in sound
xdxf = function('xdxf.cc', 'void XdxfResourceRequest::run()')
assert 'ImagePayload::tiffToBmp(data.data(),data.size())' in xdxf
assert 'if(!bmp.isEmpty())' in xdxf
assert 'Payload::assign(data,bmp.constData(),bmp.size())' in xdxf
assert not re.search(r'(?:QImage::fromData|GdTiff::tiffToQImage)\([^;]*&data\.front\(\)', xdxf)
assert 'hasAnyData=true;' in xdxf
print('SoundDir and XDXF resource wiring accepted')
PY
```

- [ ] Create three independent disposable source copies with `git archive HEAD | tar -x -C "$probe_root"`, choosing each `probe_root` with `mktemp -d /tmp/gd-resource-payload-mutations.XXXXXX`. In the first restore SoundDir's old hand-rolled `&data.front()` read. In the second restore XDXF's old `QImage::fromData(&data.front(), ...)` TIFF decode. In the third remove XDXF's `ImagePayload::tiffToBmp` call. Run the identical inline check against each copy and require nonzero status at the relevant function assertion. Do not treat a syntax/import error as a rejected wiring control. Restore each copy's changed source from the repaired checkout and rerun the same check green; record all three failure/restoration pairs.
- [ ] Review the complete diff from the specification's implementation base with `git diff 89cc501c9fbbb1ef25b9d580060083ca23a01aab -- file.cc sounddir.cc epwing.cc mdx.cc gls.cc stardict.cc slob.cc zim.cc website.cc dsl.cc xdxf.cc bgl.cc dictionary.hh dictionary.cc payload.hh imagepayload.hh imagepayload.cc bglresource.hh bglresource.cc`. Require every table row covered and all excluded functions unchanged by this repair. Preserve earlier unrelated base changes rather than treating them as new scope. Confirm the implementation checkout is unchanged with `git status --short`.
- [ ] Commit the evidence checkpoint after attaching the matrix, pointer inventory, and three negative controls:

```sh
git commit --allow-empty -m "test: record resource caller wiring audit and negative controls"
```

## Task 12: Verify both TIFF variants, the 21-suite root gate, and the feature build

**Implementer tier:** Advanced

**Lane:** resource-payload

**Category:** verification

**Files:**

- Execute/read only: `tests/tst_resource_payload/tst_resource_payload.pro`, `tests/tst_resource_payload/tst_resource_payload.cc`, `tests/tests.pro`, `goldendict.pro`, all registered modules/backends, and the committed TIFF fixture.
- Evidence: controller-owned final verification report and fresh temporary shadow-build logs; no new repository deliverable filename.

**Interfaces:**

- Consumes: the exact twenty-slot QtTest contract, the 21-suite aggregate's `check`, root `tests-run` with `tests/Makefile.tests`, the application's manual source inventory, and completed RED/mutation/wiring evidence.
- Produces: passing enabled and disabled suites, a passing serial root test gate after parallel compilation, a linked feature-enabled application with fresh object/macro evidence, and the final acceptance record.

- [ ] Check that all eight RED records, eight mutation/restoration records, and three wiring-negative-control/restoration records are present. Run enabled and disabled suites in separate fresh shadow builds, from the implementation root:

```sh
set -e
source_root=$(git rev-parse --show-toplevel)
test_build=$(mktemp -d /tmp/gd-resource-payload-test.XXXXXX)
(
  cd "$test_build"
  qmake6 "$source_root/tests/tst_resource_payload/tst_resource_payload.pro"
  make -j"$(nproc)"
  ./tst_resource_payload
)
disabled_build=$(mktemp -d /tmp/gd-resource-payload-test-no-tiff.XXXXXX)
(
  cd "$disabled_build"
  QMAKEFLAGS=CONFIG+=no_extra_tiff_handler qmake6 "$source_root/tests/tst_resource_payload/tst_resource_payload.pro" CONFIG+=no_extra_tiff_handler
  make -j"$(nproc)"
  ./tst_resource_payload
)
```

Require all twenty slots pass in each build. In enabled mode, 13 proves Qt rejection then exact 2x2 BMP pixels and 14 proves GdTiff pixels. In disabled mode, 13 expects empty conversion and 14 a null GdTiff image; Qt's initial rejection assertion remains mandatory. Keep `tiff.cc` in both source lists and require no disabled-TIFF link failure. Run directly without a display to demonstrate `QTEST_GUILESS_MAIN` behavior.
- [ ] Exercise the actual root recipe, including aggregate generation within `tests/`, parallel build, and serial `check`. Run from the implementation root in the existing test-capable environment:

```sh
set -e
qmake6 goldendict.pro
make tests-run
```

Require `tests/tests.pro: exact 21 suites`, `tests/Makefile.tests`, and successful execution of every required suite, including `tst_resource_payload`. Do not replace this with only a direct invocation of the new suite, a root-directory aggregate, or a closure archive. Record any environment/prerequisite failure as blocked acceptance with its actual diagnostic.
- [ ] Build the feature-enabled application in a fresh shadow directory, saving `build.log` there and testing every required object for nonzero size:

```sh
set -e
source_root=$(git rev-parse --show-toplevel)
build_root=$(mktemp -d /tmp/gd-resource-payload-app.XXXXXX)
(
  cd "$build_root"
  qmake6 "$source_root/goldendict.pro" PREFIX=/usr CONFIG+=chinese_conversion_support CONFIG+=zim_support
  if make -j"$(nproc)" > build.log 2>&1; then
    for object in file sounddir epwing mdx gls stardict slob zim website xdxf dsl bgl imagepayload bglresource tiff; do
      find "$build_root" -type f -name "$object.o" -size +0c -print | rg -q . || exit 1
    done
  else
    exit 1
  fi
)
```

- [ ] Inspect actual fresh compile commands in that log, not only qmake settings or object existence. Require `MAKE_ZIM_SUPPORT` on the command compiling `zim.cc`, no `NO_EPWING_SUPPORT` on the command compiling `epwing.cc`, and `MAKE_EXTRA_TIFF_HANDLER` on the command compiling `tiff.cc`. Record the paths of all fifteen non-empty objects and successful application link; a conditionally empty translation unit is not feature evidence.
- [ ] If the dictionary folder responsible for coredump 527863 has been identified, request its real zero-byte local resource through the repaired application and confirm it stays alive. Record the actual resource and result; otherwise state that the folder was not identified and retain the deterministic case-6 evidence. This conditional manual check supplements the required suite.
- [ ] Run `git diff --check` and `git status --short`; attach command/configuration results, source revision, assertion evidence, mutation/wiring results, preserved outcome matrix, and any observed implementation-order deviation. Do not claim acceptance until every mandatory gate has passed. No new failing-first case turns green here: this task verifies the repaired cases 1, 4, 6, 8, 9, 15, 16, 20 and retained controls 2, 3, 5, 7, 10–14, 17–19 in both feature variants and the full integration.
- [ ] Commit the final evidence checkpoint after submitting the report:

```sh
git commit --allow-empty -m "test: record complete resource payload acceptance verification"
```

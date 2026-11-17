# Scan popup crash on an empty resource payload

Status: implementation-ready technical specification
Date: 2026-11-16
Design: `docs/superpowers/specs/2026-11-16-scan-popup-mdd-css-crash-design.md`
Implementation base: integration worktree at `89cc501c9fbbb1ef25b9d580060083ca23a01aab`

## Problem and confirmed diagnosis

The scan popup aborted the application while displaying a dictionary article. The installed `/usr/bin/goldendict` package `2:1.5.4.r9.g30a7111d-1` was built from `30a7111d750ea1aac214b208172640e0dd10befd`, the design’s pinned application revision. This specification uses the integration base recorded above. The coredump and disassembly identify an empty-vector access in `File::loadFromFile()`, reached while `Mdx::MddResourceRequest::run()` loads a local resource:

```cpp
void loadFromFile( std::string const & n, std::vector< char > & data )
{
  File::Class f( n, "rb" );
  f.seekEnd();
  data.resize( f.tell() );
  f.rewind();
  f.read( &data.front(), data.size() );
}
```

For a zero-byte file, `resize(0)` leaves `data` empty. Evaluating `data.front()` violates the vector precondition before `File::Class::read()` can observe a zero length. The Arch package uses `-D_GLIBCXX_ASSERTIONS`, which turns this undefined access into the observed assertion and `SIGABRT`.

The binary evidence is specific: instruction address `0x3571a2` is the call to `File::loadFromFile()` at `MddResourceRequest::run()+0x342`; `0x3571a8`, reported as the caller frame, is its return address at `run()+0x348`. Inside `File::loadFromFile()`, the branch at `0x1bc1e0` reaches the empty-vector assertion block at `0xe2ad1`; the recorded `0xe2af1` frame is the return address of that block’s assertion call. The assertion strings identify `!this->empty()`, `std::vector<char>::front()`, and `/usr/include/c++/16/bits/stl_vector.h`. Other worker threads in `isolateCSS()` do not establish the cause of the crashing thread.

A second, independent defect has the same empty-vector shape. `isolateCSS()` legitimately removes comments and drops `@page` rules. When such a stylesheet becomes empty, five resource backends resize their payload to zero and then form `&data.front()` while copying the UTF-8 result: `mdx.cc:937-940`, `gls.cc:1459-1462`, `stardict.cc:1753-1756`, `slob.cc:1672-1680`, and `zim.cc:1588-1595`. These are separate from the reported local-file crash and must also be repaired.

The resource dispatcher at `article_netmgr.cc:95` can route a popup request to the selected dictionary backend. Therefore every backend row below is in scope; the fix must not assume that only MDD resources can reach the popup.

## Required contracts

### `payload.hh`

Add header-only `payload.hh`, including only `<cstddef>`, `<cstring>`, and `<vector>`. Its namespace and signatures are:

```cpp
namespace Payload {
inline void assign( std::vector< char > & destination,
                    char const * bytes, std::size_t size );
inline void append( std::vector< char > & destination,
                    char const * bytes, std::size_t size );
}
```

`assign` replaces the destination with exactly `size` bytes. It first resizes the destination. When `size == 0`, it leaves the vector empty and does not evaluate `bytes` or form/dereference an element reference. When `size > 0`, it copies exactly `size` bytes. The guarded implementation may use `&destination.front()` only inside the nonzero branch.

`append` appends exactly `size` bytes. When `size == 0`, it is a no-op even if the destination is empty; it must return before forming `&destination.front()`. When `size > 0`, save the old size, resize to `oldSize + size`, and copy to the appended region. The source storage is separate from the destination for all callers in this change.

These operations preserve binary bytes, including embedded zero bytes. Callers retain their existing success-flag and mutex behavior.

### `Dictionary::Class::setIsolatedCssPayload`

Add this public member declaration to `dictionary.hh` and implementation to `dictionary.cc`:

```cpp
void setIsolatedCssPayload( QString & css, std::vector< char > & data,
                            QString const & wrapperSelector = QString() );
```

It calls the existing protected `isolateCSS(css, wrapperSelector)`, converts the resulting `QString` with `toUtf8()`, then replaces `data` through `Payload::assign`. An empty isolated result is valid and leaves `data` empty. The method must not set or clear `hasAnyData`; that remains the caller’s responsibility. Existing `isolateCSS()` parsing and output are unchanged.

### `imagepayload.hh` and `imagepayload.cc`

Add these declarations in namespace `ImagePayload`:

```cpp
QImage decodeWithQt( char const * bytes, std::size_t size );
QImage decodeWithGdTiff( char const * bytes, std::size_t size );
QByteArray tiffToBmp( char const * bytes, std::size_t size );
```

The header includes `<cstddef>`, `QByteArray`, and `QImage`. Empty input returns a null image from either decoder and an empty `QByteArray` from `tiffToBmp`, without passing a pointer to an image reader. `decodeWithQt` delegates non-empty data to `QImage::fromData`. `decodeWithGdTiff` calls `GdTiff::tiffToQImage` only under `MAKE_EXTRA_TIFF_HANDLER`; without that define it returns a null `QImage`. `tiffToBmp` tries Qt first, then GdTiff if Qt returned a null image, and encodes a successfully decoded image as BMP. It returns empty if neither decoder succeeds or BMP encoding fails.

Both projects list `tiff.cc` unconditionally under `SOURCES`; its implementation is enclosed in `#ifdef MAKE_EXTRA_TIFF_HANDLER` (`tiff.cc:4-154`). Define `MAKE_EXTRA_TIFF_HANDLER` and link `-ltiff` only under `!CONFIG(no_extra_tiff_handler)`, as the application and `tests/testsupport.pri` already do. `tiff.cc`’s existing decoder accepts one-bit, single-sample TIFFs and maps palette index 0 to white and index 1 to black.

Callers pass the original input source: `data` for DSL, GLS, StarDict, and XDXF; the already-proven non-empty `resource` string for SLOB and ZIM. Callers replace the payload only when the returned BMP is non-empty. This preserves raw bytes in DSL/GLS/StarDict/XDXF when conversion fails, and preserves the existing empty payload in SLOB/ZIM when conversion fails. Do not call `QBuffer::constData()`; BMP bytes are a `QByteArray`, so use `bmp.constData()` and `bmp.size()`.

### `bglresource.hh` and `bglresource.cc`

Add namespace `BglResource` with these exact interfaces:

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

Include `"file.hh"`, `<cstddef>`, `<stdint.h>`, `<string>`, and `<vector>` in `bglresource.hh`; include `"bglresource.hh"`, `<cctype>`, and `<zlib.h>` in `bglresource.cc`. A directory record is exactly a 32-bit name length, that many name bytes, then a mandatory 32-bit offset. `readDirectoryEntry` resizes `entry.name`, reads its bytes only when the length is nonzero, and always reads the following offset, including for a zero-length name. It returns no status. `File::Class::read` exceptions propagate unchanged; do not catch or convert them.

`findResource` reads at most `count` records through `readDirectoryEntry`, skips an empty name only after its offset has been consumed, and returns true immediately with the first matching record’s offset when its lowercased name equals `lowercaseName`. Use BGL’s existing bytewise `tolower` rule for directory names and the request name. Empty directory names are never matches, including for an empty requested name. On no match it consumes all `count` records and returns false; `offset` is unchanged. The existing BGL request remains responsible for `idx.seek(offset)` and resource-block reads.

`decompress` resizes `data` to `advertisedSize` and accepts an empty output as a valid result. If `advertisedSize == 0`, pass zlib a one-byte scratch destination with initial capacity 1; after `uncompress`, success requires both `Z_OK` and actual produced length 0. For nonzero advertised size, decode into `data` and require both `Z_OK` and actual produced length exactly equal to `advertisedSize`. Empty compressed input returns false before any `front()` access. Corrupt data, a short output, and an output longer than the advertised size return false. On false, the caller does not publish `hasAnyData = true`.

`BglResourceRequest::run()` retains its initial cancellation check, request-name lowercasing, outer `idxMutex` scope, and `idx.seek(resourceListOffset)`. Retain the `for (size_t count = resourcesCount; count--;)` loop and its per-record `isCancelled.loadAcquire()` check (`bgl.cc:1040-1043`). Each iteration calls `findResource(idx, 1, nameLowercased, offset)`; false continues to the next record, while true seeks to `offset`, acquires `dataMutex`, reads the advertised and compressed sizes, reads non-empty compressed bytes, and calls `decompress`. On false decompression retain the existing `gdWarning`; on true publish `hasAnyData = true`, including for a valid zero-byte stream. Break after processing the first matching record regardless of decompression success, and retain the final `finish()`. Calling the seam with one record at a time preserves the existing cancellation point without adding a callback or changing its approved signature.

No exception handler is added around index reads. A truncated index record still propagates `File::Class::read`’s `exReadError`; the corrupt-stream fixture is fully present in storage, so it tests zlib rejection rather than short file I/O.

## Production-site change table

All edits below are in the named resource request or loader. Preserve the existing catch blocks, lock scopes, lookup precedence, and `hasAnyData` assignment point unless a row explicitly says otherwise.

| Site | Required edit | Empty-safety and outcome condition |
| --- | --- | --- |
| `file.cc:55-65` | After `data.resize(f.tell())` and `f.rewind()`, call `f.read(&data.front(), data.size())` only inside `if ( !data.empty() )`. | A successfully opened zero-byte file returns an empty vector. `File::loadFromFile` does not decide request success. |
| `sounddir.cc:372-384` | Replace its hand-rolled `File::Class` size/rewind/read block with `File::loadFromFile(FsEncoding::encode(fileName), data)`. Keep the surrounding `DataRequestInstant(true)` and `catch (File::Ex&)` behavior. | Existing empty sound file remains successful with an empty payload; missing file remains `DataRequestInstant(false)`. |
| `epwing.cc:780-788` | Replace resize plus `memcpy(&data.front(), buffer.data(), ...)` with `Payload::assign(data, buffer.constData(), buffer.size())`. Keep `hasAnyData = true` inside the successful open branch. | Existing empty cache file remains successful with size 0. |
| `mdx.cc:937-940` | Replace `isolateCSS` + `toUtf8` + resize/copy with `dict.setIsolatedCssPayload(css, data, ".mdict")`. | Keep the earlier `hasAnyData = true` at `mdx.cc:900`; a non-empty raw resource transformed to empty remains success with `dataSize() == 0`. An initially empty resource still skips that existing flag assignment. |
| `gls.cc:1459-1462` | Replace CSS isolation and byte copy with `dict.setIsolatedCssPayload(css, data)`. | Keep the existing later `hasAnyData = true`; transformed-empty is success with size 0. |
| `stardict.cc:1753-1756` | Replace CSS isolation and byte copy with `dict.setIsolatedCssPayload(css, data)`. | Keep existing flag assignment; transformed-empty is success with size 0. Preserve StarDict’s existing URL rewrite before isolation. |
| `slob.cc:1672-1680` | Replace CSS isolation and byte copy with `dict.setIsolatedCssPayload(css, data, ".slobdict")`. | Keep the existing check that rejects empty `resource` at `slob.cc:1669`; an initially empty/missing resource stays unsuccessful, while non-empty CSS transformed to empty is successful with size 0. |
| `zim.cc:1588-1595` | Replace CSS isolation and byte copy with `dict.setIsolatedCssPayload(css, data, ".zimdict")`. | Keep the existing empty-resource rejection at `zim.cc:1584` and later success assignment. |
| `website.cc:352-359` | Replace `prevSize`, `data.resize`, and `memcpy(&data.front() + prevSize, ...)` with `Payload::append(data, cssData.constData(), cssData.size())`. Keep the current data mutex scope and `hasAnyData = true`. | An empty reply or CSS transformed to empty appends zero bytes but remains a successful completed response under the existing flag rule. Do not change the article-body append at `website.cc:203`; its fixed wrapper and cleaner make it non-empty. |
| `xdxf.cc:1132-1163` | Pass the original `data.data(), data.size()` to `ImagePayload::tiffToBmp`. If BMP is non-empty, replace the vector with `Payload::assign(data, bmp.constData(), bmp.size())`. Do not call `QImage::fromData` or GdTiff with `&data.front()`. | Empty input never forms an element address through `front()`; a non-empty undecodable source remains raw. Do not add an assignment for an empty conversion. |
| `dsl.cc:2034-2062` | Apply the same `ImagePayload::tiffToBmp` and non-empty `Payload::assign` rule using `data` as the source. | Preserve the raw source when conversion fails. The separate article-image probe at `dsl.cc:1001-1011` is excluded because its `imgdata` is guarded at `dsl.cc:987`. |
| `gls.cc:1384-1414` | Apply the same image conversion and non-empty replacement rule using `data`. | Preserve original non-empty undecodable image bytes. |
| `stardict.cc:1678-1708` | Apply the same image conversion and non-empty replacement rule using `data`. | Preserve original non-empty undecodable image bytes. |
| `slob.cc:1686-1712` | Decode the non-empty source string `resource`, not the still-empty destination vector. Use `ImagePayload::tiffToBmp(resource.data(), resource.size())`; replace `data` only for a non-empty BMP. | `resource.empty()` is rejected at `slob.cc:1669`. A non-empty undecodable TIFF leaves `data` empty but still follows the existing success assignment. The current `GdTiff::tiffToQImage(&data.front(), ...)` at `slob.cc:1693` must disappear. |
| `zim.cc:1601-1627` | As for SLOB, use the non-empty source string `resource` for conversion and assign only a non-empty BMP. | Empty `resource` is rejected at `zim.cc:1584`; non-empty undecodable image remains a successful empty payload. Remove the `&data.front()` fallback at `zim.cc:1608`. |
| `slob.cc:1715-1718` | Replace the defensive archive payload resize/copy with `Payload::assign(data, resource.data(), resource.size())`. | This code follows the non-empty resource check, so this is defensive cleanup; the source is non-empty here. |
| `zim.cc:1630-1633` | Replace the defensive archive payload resize/copy with `Payload::assign(data, resource.data(), resource.size())`. | This code follows the non-empty resource check; preserve its existing success flag. |
| `bgl.cc:1040-1053` | Replace the inline record read/lowercase/match body with `BglResource::findResource(idx, 1, nameLowercased, offset)`. Retain the outer record loop and its cancellation check as specified above. | A zero-length name consumes its offset before being skipped; a later valid record remains discoverable. |
| `bgl.cc:1061-1078` | Read the first resource-block `uint32_t` into `advertisedSize`; read the next `uint32_t` as the compressed byte count and construct `vector<char> compressedData` of that size. Read compressed bytes only when that vector is non-empty. Replace the inline output resize and `uncompress` block with `BglResource::decompress(compressedData, advertisedSize, data)`. Set `hasAnyData = true` only when it returns true. | A valid zero-output stream remains successful; empty compressed input and malformed or length-mismatched streams remain unsuccessful. |
| `bgl.cc:933-935` | No edit to this article-request copy. | The article result is distinct from the resource-specific directory and decompression sites above and remains outside this repair’s scope. |
| `article_netmgr.cc:95` | No code edit; retain generic dispatch. | This is the popup reachability point, not a payload formation site. |

The XDXF article copy at `xdxf.cc:657` is excluded: `xdxf.cc:619` returns when both article maps are empty, and every surviving article loop appends the non-empty cleaner established at `xdxf.cc:630` before the copy. The website article append at `website.cc:203` is excluded because the body contains the fixed wrapper and cleaner strings. The article chunk reader at `xdxf.cc:689` and `chunkedstorage.cc:179` are outside resource-payload formation; the latter permits a successfully decoded empty chunk, which resource callers must handle independently. BGL short index reads are also excluded from this repair’s outcome contract as specified above.

## Empty-result and `dataSize()` contract

`Dictionary::DataRequest::dataSize()` returns `data.size()` when `hasAnyData` is true and `-1` otherwise (`dictionary.cc:112`). Preserve that distinction. A finished request with `dataSize() == 0` is a successful empty resource; `-1` means not found or rejected. Do not infer success from `data.empty()` and do not set `hasAnyData` in a shared helper.

| Backend and condition | Existing flag behavior after repair | Final `dataSize()` |
| --- | --- | ---: |
| MDD: existing local file is zero bytes | `File::loadFromFile` returns empty; MDD’s existing `data.size() > 0` gate at `mdx.cc:900` does not set the flag. | `-1` |
| MDD: no local file and no matching MDD resource | No data flag is set. | `-1` |
| MDD: non-empty CSS becomes empty under isolation | MDD sets `hasAnyData` before CSS transformation at `mdx.cc:900`. | `0` |
| DSL, GLS, StarDict, XDXF: successfully loaded resource is initially empty | The request’s existing success publication remains after resource processing. `IndexedZip::loadFile` accepts an empty stored entry. | `0` |
| DSL, GLS, StarDict, XDXF: non-empty image is undecodable | Conversion result is not assigned; original raw bytes remain; existing request sets success. | Original positive byte count |
| DSL, GLS, StarDict, XDXF: empty image input | No image pointer is formed; no BMP is assigned; the existing request success assignment remains. | `0` |
| GLS, StarDict, SLOB, ZIM: non-empty CSS becomes empty | The existing success assignment remains after processing. | `0` |
| SLOB or ZIM: source resource string is initially empty | Existing `exNoResource`/`File::Ex` rejection runs before CSS, image, or archive copies and before success publication. | `-1` |
| SLOB or ZIM: non-empty image source is undecodable | No BMP is assigned; destination remains empty; existing success publication remains. | `0` |
| SoundDir: indexed sound file exists and has zero bytes | `DataRequestInstant(true)` remains the request result. | `0` |
| SoundDir: indexed sound file is missing | Existing `catch (File::Ex&)` returns `DataRequestInstant(false)`. | `-1` |
| EPWING: cache file opens and has zero bytes | Existing flag assignment remains inside the successful file-open branch. | `0` |
| EPWING: cache path is unavailable or file does not open | Existing path returns without setting the data flag. | `-1` |
| Website: successful stylesheet response becomes empty or has zero bytes | Zero-byte append is a no-op; existing success assignment remains. | `0` |
| Website: request fails | Existing error and no-data behavior remains. | `-1` |
| XDXF ZIP: zero-length stored, non-TIFF entry | `IndexedZip::loadFile` clears the vector and returns true (`indexedzip.cc:78-81`); XDXF publishes success at `xdxf.cc:1167`. | `0` |
| XDXF ZIP: zero-length stored TIFF entry | Empty input goes through `ImagePayload::tiffToBmp`; empty conversion is not assigned; XDXF publishes success. | `0` |
| SLOB or ZIM: empty archive entry | The loaded empty `resource` is rejected before the archive copy. | `-1` |
| BGL: fully readable record with valid zlib stream producing zero bytes | `decompress` succeeds using scratch storage; existing success publication runs. | `0` |
| BGL: fully readable record with empty compressed input, corrupt stream, or output length mismatch | `decompress` returns false; success is not published. | `-1` |
| BGL: short/truncated directory or compressed bytes in the index file | `File::Class::read` throws as before; request exception behavior is unchanged and no new `-1` guarantee is made here. | Unchanged / no completed outcome specified |

`IndexedZip::loadFile`’s empty stored-entry outcome is provided by the archive reader, not by `Payload::assign`; the XDXF TIFF row likewise relies on `ImagePayload::tiffToBmp` returning empty and XDXF’s existing success publication.

## Application build registration

`goldendict.pro` manually enumerates headers and sources. Register:

- `payload.hh` under `HEADERS`;
- `imagepayload.hh` under `HEADERS` and `imagepayload.cc` under `SOURCES`;
- `bglresource.hh` under `HEADERS` and `bglresource.cc` under `SOURCES`.

Place each entry beside its related existing file (`payload.hh` near `file.hh`, `imagepayload.hh` near `tiff.hh`, and `bglresource.hh/.cc` near `bgl.hh/.cc`). The application must compile and link both new translation units; suite-only registration is insufficient.

The application uses `MAKE_ZIM_SUPPORT` only under `CONFIG(zim_support)` and includes EPWING unless `no_epwing_support` is set. TIFF support is enabled unless `no_extra_tiff_handler` is set. The feature-enabled application verification therefore explicitly adds `CONFIG+=chinese_conversion_support CONFIG+=zim_support` while leaving EPWING and TIFF enabled.

## Regression suite: `tests/tst_resource_payload`

Create `tests/tst_resource_payload/tst_resource_payload.pro` and `tst_resource_payload.cc`. This is a QCoreApplication suite with `QTEST_GUILESS_MAIN`; it must run without a display even though `tests/testsupport.pri` links Qt Widgets and WebEngine modules. Add `QMAKE_CXXFLAGS += -D_GLIBCXX_ASSERTIONS` to this suite so extraction-phase empty-vector accesses abort with the same libstdc++ assertion class as the package binary.

Use this complete `tst_resource_payload.pro`. The shared include supplies `MAKE_EXTRA_TIFF_HANDLER` unless explicitly disabled; `dictionary.hh` must be MOCed because its request classes carry `Q_OBJECT`.

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

Define `ResourcePayloadTests: public QObject` with `Q_OBJECT` and the 20 `private slots` below. End the suite source with:

```cpp
QTEST_GUILESS_MAIN( ResourcePayloadTests )
#include "tst_resource_payload.moc"
```

Define suite-local `Config` substitutes as in `tst_btree_index.cc`:

```cpp
namespace Config {
bool isPortableVersion() throw() { return false; }
QString getPortableVersionDictionaryDir() throw() { return QString(); }
}
```

Define `ResourcePayloadDictionary` as the concrete `Dictionary::Class` fixture with these complete signatures and inert implementations:

```cpp
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
```

Keep `isolateCSS()` protected and invoke the new public `setIsolatedCssPayload()` through this fixture. Include `tests/support/fixtures.hh` as `"fixtures.hh"` and load the TIFF fixture with `QFile` from `TestSupport::repositoryRoot() + "/tests/fixtures/imagepayload/tiny-1bit-no-photometric.tiff"` so both shadow builds read the same committed bytes. For slots 8 and 9 seed `data` with the three bytes `6f 6c 64` before isolation and assert the seed is removed.

The suite defines exactly these test slots and assertions:

| # | Test slot | Required assertion and phase |
|---:|---|---|
| 1 | `payloadAssign_emptyLeavesVectorEmpty` | Seed a non-empty vector, call `Payload::assign(data, nullptr, 0)`, assert empty. Failing-first. |
| 2 | `payloadAssign_copiesExactBytes` | Assign bytes `41 00 ff`; assert size 3 and exact byte equality. Control. |
| 3 | `payloadAssign_replacesLongerContent` | Replace a longer seeded vector with bytes `78 79`; assert exactly two bytes and no stale suffix. Control. |
| 4 | `payloadAppend_emptyIsNoOp` | Start with an empty vector, append `nullptr, 0`, assert empty. Failing-first. |
| 5 | `payloadAppend_appendsExactly` | Start with byte `61`, append `62 63`, assert exact bytes `61 62 63`. Control. |
| 6 | `loadFromFile_zeroByteFileIsEmpty` | Create an existing zero-byte file in `QTemporaryDir`, load it through `File::loadFromFile`, assert empty. Failing-first; this reproduces the reported access and covers SoundDir’s delegated loader. |
| 7 | `loadFromFile_readsExactBytes` | Write `00 ff 31 00`, load it, and assert exact four-byte equality. Control. |
| 8 | `setIsolatedCssPayload_commentOnlyYieldsEmpty` | Input `/* only */`; assert mutated CSS and payload are empty. Failing-first. |
| 9 | `setIsolatedCssPayload_pageOnlyYieldsEmpty` | Input `@page { size: A4; }`; assert mutated CSS and payload are empty. Failing-first. |
| 10 | `setIsolatedCssPayload_prefixesOrdinaryRules` | Input `.entry { color: red; }`; assert CSS is exactly `#gdfrom-resource-payload-test .entry { color: red; }` and payload is its exact UTF-8 byte sequence. Control. |
| 11 | `tiffToBmp_emptyInputYieldsEmpty` | Assert both decoders return null images and `tiffToBmp(nullptr, 0)` is empty. Control. |
| 12 | `tiffToBmp_undecodableInputYieldsEmpty` | Use bytes `00 01`; assert Qt decode is null and conversion is empty. Control. |
| 13 | `tiffToBmp_producesBmpFromValidTiff` | Load the committed no-photometric fixture below; first assert Qt rejects it, then assert enabled conversion returns a BMP decoding to the exact 2x2 grid below. With TIFF support disabled, assert empty conversion. Control. |
| 14 | `gdTiffDecodesCommittedOneBitFixture` | Call `decodeWithGdTiff` directly. With TIFF support enabled, assert non-null 2x2 image and the exact grid; disabled, assert null. Control. |
| 15 | `bglFindResource_emptyNameSkippedAfterOffset` | After the guard phase, read the two-record fixture below; assert an empty requested name does not match, then assert `image.tif` resolves to offset `0x55667788`. Failing-first extraction run aborts while reading the empty name. |
| 16 | `bglDecompress_validEmptyStreamSucceedsEmpty` | After the guard phase, decompress the valid empty zlib stream below with advertised size 0; assert true and empty output. Failing-first extraction run aborts while forming the zero-length output address. |
| 17 | `bglDecompress_corruptStreamFails` | Decompress bytes `78 9c 03 00 00 00 00 00` with advertised size 1; assert false. Control: a non-empty destination lets the extraction reach zlib’s checksum rejection without first aborting on output `front()`. |
| 18 | `bglDecompress_exactNonEmptyRoundTrip` | Decompress `78 9c 4b 4c 4a 06 00 02 4d 01 27` with advertised size 3; assert true and bytes `61 62 63`. Control. |
| 19 | `bglDecompress_advertisedLengthMismatchFails` | Decompress the `abc` stream in separate calls with advertised sizes 2 and 4; assert false for each. The size-2 case exhausts output capacity; size 4 detects a short successful zlib output. Control. |
| 20 | `bglDecompress_emptyCompressedInputFails` | After the guard phase, use empty compressed storage with advertised sizes 1 and 0 in that order; assert false without abort for each. The extraction’s first call has a non-empty output vector and aborts on the empty compressed-vector address. |

### Committed TIFF fixture

Commit the fixture under `tests/fixtures/imagepayload/`:

- `tiny-1bit-no-photometric.tiff`: little-endian classic TIFF. Header bytes are `49 49 2a 00 08 00 00 00`; IFD has 9 entries and next-IFD offset 0. Tags: 256 ImageWidth LONG 2; 257 ImageLength LONG 2; 258 BitsPerSample SHORT 1; 259 Compression SHORT 1; 273 StripOffsets LONG 122; 277 SamplesPerPixel SHORT 1; 278 RowsPerStrip LONG 2; 279 StripByteCounts LONG 2; 284 PlanarConfiguration SHORT 1. The two strip bytes are `40 80`. Omit tag 262 `PhotometricInterpretation` intentionally. Qt’s TIFF reader must reject this fixture; GdTiff accepts it.

For the fixture, the expected GdTiff pixel grid is row 0 `[white, black]`, row 1 `[black, white]`, because `0x40` and `0x80` encode the scanlines and GdTiff maps bit value 0 to white and 1 to black. The exact file bytes, grouped by file offset, are:

```text
tiny-1bit-no-photometric.tiff
0000: 49 49 2a 00 08 00 00 00 09 00 00 01 04 00 01 00
0010: 00 00 02 00 00 00 01 01 04 00 01 00 00 00 02 00
0020: 00 00 02 01 03 00 01 00 00 00 01 00 00 00 03 01
0030: 03 00 01 00 00 00 01 00 00 00 11 01 04 00 01 00
0040: 00 00 7a 00 00 00 15 01 03 00 01 00 00 00 01 00
0050: 00 00 16 01 04 00 01 00 00 00 02 00 00 00 17 01
0060: 04 00 01 00 00 00 02 00 00 00 1c 01 03 00 01 00
0070: 00 00 01 00 00 00 00 00 00 00 40 80
```

Case 13 uses the no-photometric fixture so a passing conversion proves the Qt-rejected path reaches GdTiff. Case 14 directly checks the decoder and pixel oracle. Case 13’s enabled run must decode the resulting BMP with `QImage::fromData(reinterpret_cast<uchar const *>(bmp.constData()), bmp.size(), "BMP")` and compare dimensions and all four `QColor` values. Do not weaken the initial Qt-rejection assertion into a skip.

### BGL fixture bytes

Build the BGL directory fixture in memory and write it to a `QTemporaryDir` file as these 25 bytes (32-bit fields are little-endian on the supported test target):

```text
00 00 00 00   01 00 00 00
09 00 00 00   69 6d 61 67 65 2e 74 69 66   88 77 66 55
```

This is an empty-name entry with offset 1, followed by a length-9 `image.tif` entry with offset `0x55667788`. Open it with `File::Class`, seek to zero before each independent search, and call `findResource` with count 2. First assert an empty requested name returns false, then rewind and assert `image.tif` returns true with offset `0x55667788`. The deliberately small first offset ensures a reader that fails to consume it parses a bounded wrong record and returns the wrong result rather than allocating from a huge malformed length.

Use these exact zlib fixtures:

- Valid empty stream: `78 9c 03 00 00 00 00 01` (8 bytes; `Z_OK`, produced length 0).
- Corrupt checksum: `78 9c 03 00 00 00 00 00` (`Z_DATA_ERROR`).
- Valid `abc`: `78 9c 4b 4c 4a 06 00 02 4d 01 27` (11 bytes; output `61 62 63`).
- Empty compressed storage: zero bytes; false without calling `uncompress` with an invalid empty-vector element address.

### Failing-first phase, mutations, and wiring audit

The first implementation phase extracts the seams and rewires callers while preserving the unsafe zero-length bodies in `Payload::assign`, `Payload::append`, `File::loadFromFile`, `setIsolatedCssPayload`, and the two BGL helpers. Build with `_GLIBCXX_ASSERTIONS`, then run failing-first slots individually: 1, 4, 6, 8, 9, 15, 16, and 20. Each must terminate with the libstdc++ empty-vector assertion and `SIGABRT`. Cases 15, 16, and 20 are explicitly failing-first, not controls: current `bgl.cc` forms `&nameData.front()` for an empty name at approximately `bgl.cc:1046` and `&data.front()` for zero output at `bgl.cc:1069`; extraction without guards preserves those failures. Slots 2, 3, 5, 7, 10-14, and 17-19 must pass in both phases. After recording those results, add the zero-length guards and exact-length BGL checks; all 20 slots must pass.

Run these isolated mutations on a disposable copy of the repaired tree. Each mutation must begin from a passing baseline; record the named test failure, restore the source, and rerun that test green before moving to the next mutation.

| Mechanism | Mutation | Required killing evidence |
|---|---|---|
| `Payload::assign` | Remove the nonzero-size guard while retaining the `front()` copy. | Slot 1 aborts. |
| `Payload::append` | Remove the zero-size early return while retaining the `front()` append address. | Slot 4 aborts. |
| `File::loadFromFile` | Remove the `!data.empty()` read guard. | Slot 6 aborts. |
| `setIsolatedCssPayload` | Restore the old resize plus `&data.front()` copy for an empty UTF-8 result. | Slots 8 and 9 abort. |
| `BglResource::readDirectoryEntry` / `findResource` | Continue on an empty name before consuming its mandatory offset. | Slot 15 fails to find `image.tif`. |
| `BglResource::decompress` | Reject or mishandle a valid zero-byte output. | Slot 16 fails. |
| `BglResource::decompress` | Remove `actualLength == advertisedSize`. | Slot 19 incorrectly succeeds. |
| `ImagePayload::tiffToBmp` | Remove its GdTiff fallback call. | Slot 13 fails after first proving Qt rejected the fixture. |

Slot 20 is not a guard-removal mutation: it pins the invalid-input result and is failing-first only because the behavior-preserving extraction retains the old empty `front()` formation. Slot 17 deliberately uses advertised size 1, keeping its checksum rejection a control in both phases. Slot 19 covers both undersized and oversized advertised lengths; removing the equality check must make the size-4 assertion fail. `ImagePayload::tiffToBmp` takes a pointer and size and does not need a vector `front()` guard; do not claim that removing an empty-input check must abort.

The suite’s closure cannot execute every production request function without linking the full dictionary formats. Therefore, also complete a function-scoped wiring audit against the change table before acceptance:

1. Verify each listed resource request calls the named seam at the exact payload construction point, and that the old unguarded `resize`/`front`/copy or decode-pointer sequence is absent from that resource function. In particular inspect `QImage::fromData(&data.front(), ...)`, `GdTiff::tiffToQImage(&data.front(), ...)`, empty BMP assignments, `website.cc`’s append address, and BGL’s name/compressed/output vectors; a general search for `memcpy` alone is insufficient.
2. Verify `sounddir.cc` delegates its file load to `File::loadFromFile` and retains its success/missing branches; verify `bgl.cc` delegates both the directory walk and decompression to `BglResource` and publishes success only on a true decompression result.
3. Verify XDXF’s empty stored ZIP TIFF reaches `ImagePayload::tiffToBmp(data.data(), data.size())`, that an empty BMP is not assigned, and that the existing XDXF success flag remains. Verify SLOB/ZIM pass their proven non-empty `resource` strings into the same seam and no longer pass the empty destination vector to the fallback decoder.
4. As negative controls for this audit, on disposable copies restore the old SoundDir `&data.front()` read and restore XDXF’s `QImage::fromData(&data.front(), ...)` in its TIFF branch. The function-scoped check must reject each copy; restore the passing tree and recheck both sites. Also remove the XDXF `tiffToBmp` call in a disposable copy and confirm the wiring check rejects it. These controls prove the audit observes the independent guards and caller wiring that the linked unit suite cannot invoke.
5. Enumerate remaining resource-payload pointer formations in `mdx`, `gls`, `stardict`, `slob`, `zim`, `website`, `dsl`, `xdxf`, `bgl`, `epwing`, `sounddir`, and `file`. Each remaining formation must either be inside an explicit non-empty branch or be unrelated to this resource path and named in the exclusions above. `mdx.cc:887`’s redirection `front()` is protected by `data.size() > sizeof(pattern)`; do not remove or weaken that check.

## Aggregate test inventory

Add `tests/tst_resource_payload/tst_resource_payload.pro` to the live suite discovery. `tests/tests.pro` currently enforces an exact 20-project inventory. Update every live count pin to 21:

- the header comment from “exactly the 20 required suites” to “exactly the 21 required suites”;
- insert `tst_resource_payload` between `tst_regex` and `tst_save_article` in `GD_REQUIRED_SUITE_NAMES`;
- change `!count(GD_TEST_NAMES, 20)` to 21 and change its error text to “Expected exactly 21 required suites”;
- change the status message to `tests/tests.pro: exact 21 suites`.

Update the `goldendict.pro` `tests-build` comment from “all 20 suites” to “all 21 suites”. Do not alter historical plans or specifications that record the earlier inventory.

Preserve the root test-gate recipe: `CHECK_MAKEFILE = Makefile.tests`; `tests-makefile` generates `tests/Makefile.tests` from `tests/tests.pro`; `tests-build` builds it in parallel; `tests-run` invokes its `check` target serially; root `check` depends on `tests-run`. The aggregate must be generated in `tests/`, not the repository root, so each suite’s `.moc` rules remain available.

## Verification commands

Run each shell block from the implementation worktree root after the code and project edits. These are implementation acceptance commands; authoring this specification does not run them.

```sh
set -e
qmake6 goldendict.pro
make tests-run
```

This exercises the live root gate, including generation of `tests/Makefile.tests`, parallel compilation of all 21 suite projects, and serial `check` execution. Configure separate fresh shadow builds for both TIFF configurations. Pass the disabled CONFIG setting as a qmake argument so it reaches the project as well as recording it in `QMAKEFLAGS`:

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

The assertion-enabled build must preserve the QtTest slot names listed above. With `CONFIG+=no_extra_tiff_handler`, slots 13 and 14 must expect empty conversion and a null GdTiff image respectively. To collect pre-guard evidence, configure a separate extraction build and run each failing-first slot individually:

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

This records eight individual assertion exits and checks all twelve controls. Run this block during extraction before guards, then use the complete suite after guards. Record the assertion text as well as exit status 134; another cause of `SIGABRT` does not satisfy the evidence requirement.

For the application feature build, use a fresh shadow directory and save its build log. Object paths are under that directory; require each named object to exist and be non-empty:

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

In addition to these object files, inspect the fresh compile commands and require `MAKE_ZIM_SUPPORT` for `zim.cc`, the absence of `NO_EPWING_SUPPORT` for `epwing.cc`, and `MAKE_EXTRA_TIFF_HANDLER` for `tiff.cc`. A disabled translation unit can still produce a non-empty object, so file existence alone is insufficient.

Also perform every mutation in the table with a passing baseline and a clean rerun after restoration, and complete the function-scoped wiring audit and its negative controls. If the dictionary folder that produced coredump 527863 is identified, add a final manual check that requests its zero-byte local resource and confirms the application remains alive; this supplements the deterministic suite.

## Acceptance criteria

- A zero-byte local resource no longer aborts through `File::loadFromFile`; SoundDir uses that guarded loader.
- Empty assignment, append, CSS isolation, image input, BGL name parsing, and BGL zero-output decompression obey the contracts above.
- Every resource-payload site in the production change table is wired to its specified seam or guard; no listed decode path passes an unproven empty-vector element address.
- All 20 `tst_resource_payload` slots pass after the guard phase; the eight failing-first slots abort on the behavior-preserving extraction under `_GLIBCXX_ASSERTIONS`.
- Every mutation has the stated killing evidence and a restored green control; the source wiring audit rejects its three negative controls.
- The exact 21-suite aggregate gate and feature-enabled application build pass, with fresh object evidence for every touched backend and both new implementation files.
- The TIFF-disabled suite compiles and has the prescribed conditional outcomes.
- Non-empty payload bytes, image fallback behavior, request lookup order, and successful-empty versus missing `dataSize()` outcomes remain unchanged.

## Implementation order

1. **Extraction phase:** add `payload.hh`, `imagepayload.hh/.cc`, and `bglresource.hh/.cc`; add `setIsolatedCssPayload`; wire all production sites; add the TIFF and BGL fixtures, test suite, qmake source registrations, and 21-suite inventory edits. Preserve the existing zero-length `front()` formations in the extracted `Payload`, file-loader, CSS-copy, and BGL bodies long enough to run the failing-first probes. Do not classify BGL cases 15, 16, or 20 as extraction controls.
2. **RED evidence:** build the assertion-enabled suite and run slots 1, 4, 6, 8, 9, 15, 16, and 20 individually. Record each assertion and exit status. Confirm controls pass before adding guards.
3. **Guard phase:** guard zero-length `Payload` operations and file reads; make empty CSS assignment safe; use size-aware image conversion and only assign non-empty BMP output; consume BGL offsets before skipping empty names; implement scratch-backed valid-empty decompression, invalid-empty-input rejection, and exact advertised-length validation.
4. **Verification phase:** run the complete suite, mutation table, function-scoped wiring audit and negative controls, both TIFF feature variants, the 21-suite root gate, and the fresh feature-enabled application build. Report any deviation from this order with its observed evidence.

## Specification notes

- **Binary offsets:** use `run+0x342` for the call at `0x3571a2` and `run+0x348` for the return address at `0x3571a8`. Earlier design text had both relative offsets six bytes too large; the objdump/coredump evidence and accepted review correction govern this specification.
- **Live source line range:** the design cites `file.cc:57-66`; in the current integration tree `File::loadFromFile` starts at line 55, and the unsafe `front()` is line 65. The edit table uses live lines `55-65`.
- **Dictionary fixture signatures:** the live `Dictionary::Class` has six pure virtuals at `dictionary.hh:307`, `:311`, `:319`, `:323`, `:346`, and `:384`. The suite must implement all six, even if an earlier design revision mentioned only four.
- **BGL phase classification:** faithful extraction retains the unsafe empty-name and zero-output `front()` expressions. Cases 15 and 16 therefore belong to the failing-first guard phase, as does case 20. They cannot be controls required to pass before guards.
- **BGL caller and control inputs:** preserve the per-record cancellation at `bgl.cc:1040-1043` by calling `findResource` with count 1 inside the existing loop. Change the call-site compressed storage from `vector<unsigned char>` (`bgl.cc:1063`) to the approved `vector<char>` seam type, guarding its read. Case 17 uses advertised size 1 so the corrupt checksum reaches zlib during extraction; case 19 checks both advertised sizes 2 and 4; case 20 checks empty compressed storage with nonzero and zero advertised sizes. These choices preserve the design’s eight failing-first cases and twelve controls.
- **TIFF link closure and fixture:** the test project must list `tiff.cc` unconditionally and conditionally link `-ltiff`; `tests/testsupport.pri` enables the extra handler by default. `goldendict.pro:545,621-626` and `tests/tst_utf8_wstring/tst_utf8_wstring.pro:35,51` establish that disabled TIFF still compiles an empty `tiff.cc`. The no-photometric fixture’s Qt rejection is asserted before fallback output, avoiding a host-dependent pass through Qt’s TIFF plugin.
- **QBuffer API:** use `QByteArray::constData()` for BMP bytes. `QBuffer` itself has no `constData()` method.
- **SoundDir API:** the live implementation uses `File::Class` directly at `sounddir.cc:372-384`, not `QFile`; its existing missing-resource catch returns an unsuccessful instant request. The required edit delegates to the shared `File::loadFromFile` guard.
- **Archive and outcome attribution:** zero-length XDXF ZIP success comes from `IndexedZip::loadFile` at `indexedzip.cc:78-81` and XDXF’s existing success flag, not from `Payload::assign`. SLOB/ZIM reject empty resource strings before their archive copies; their `resource` variable is the non-empty source for image conversion.
- **Test inventory:** the live qmake files independently pin the count in the header comment, required name list, count predicate and error, status message, and `goldendict.pro` aggregate comment. Update all of them to 21.
- **Scope:** `article_netmgr.cc:95` is the generic dispatcher; XDXF’s article copy, website’s fixed-wrapper article append, the guarded DSL article-image probe, BGL short-read exception behavior, and unrelated article/storage pointer sites are not altered by this resource-payload repair.

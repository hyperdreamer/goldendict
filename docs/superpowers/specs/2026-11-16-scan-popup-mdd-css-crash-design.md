# Scan popup crash on an empty resource payload

Status: draft for review
Date: 2026-11-16
Run: pm-run-20261116-203148-068023b6
Target: `refs/heads/master` at `30a7111d750ea1aac214b208172640e0dd10befd`

## Background

The scan popup aborted the whole application (`SIGABRT`, coredump) while a dictionary
article was being displayed. The crashing thread's stack was:

```
abort
__glibcxx_assert_fail
goldendict + 0xe2af1                     # std::vector<char>::front()
Mdx::MddResourceRequest::run()           # mdx.cc
QThreadPool / QRunnable
```

The installed binary (`/usr/bin/goldendict`, package `2:1.5.4.r9.g30a7111d-1`) was
built from exactly the commit pinned above, so the defect is present in `master`.

The assertion's own strings identify the failing access precisely: the expression
`!this->empty()`, the location `/usr/include/c++/16/bits/stl_vector.h`, and the
function `std::vector<_Tp, _Alloc>::reference std::vector<_Tp, _Alloc>::front()
[with _Tp = char]`. In other words a `std::vector<char>` was asked for its first byte
while it was empty. Arch's default `makepkg` flags include `-D_GLIBCXX_ASSERTIONS`, so
this undefined access aborts instead of reading past the end silently.

## Root cause

The scan popup renders an article whose HTML references the dictionary's bundled
resources with `bres://` URLs. `ArticleSchemeHandler` asks
`ArticleNetworkAccessManager`, which asks the dictionary, and an MDD resource is served
by `MddResourceRequest::run()` (`mdx.cc:817`).

For a stylesheet resource that request does this:

1. It reads the resource bytes (a local file first, then the `.mdd` archive), and
   `mdx.cc:937` calls `dict.isolateCSS( css, ".mdict" )`.
2. `Dictionary::Class::isolateCSS()` (`dictionary.cc:276`) intentionally reduces a
   stylesheet: it strips comments and deliberately drops `@page` rules, among other
   rewrites. A stylesheet that contains only comments, or only `@page` rules, therefore
   isolates to an **empty** `QString`. That is intended output, not an error.
3. `mdx.cc:938-940` then does:

   ```cpp
   QByteArray bytes = css.toUtf8();
   data.resize( bytes.size() );          // becomes 0 for an empty stylesheet
   memcpy( &data.front(), bytes.constData(), bytes.size() );
   ```

   `&data.front()` is evaluated before the zero-length `memcpy`; on the now-empty
   vector that is an out-of-range access, and with libstdc++ assertions it aborts.

The stack shows several other threads inside `isolateCSS()` at the same time because
the popup issues several resource requests concurrently. Only the request whose
stylesheet isolated to nothing reached the invalid access, which is why the popup
usually works and only some dictionaries crash it.

### The same idiom elsewhere on the popup's path

The identical "resize to N, then touch the first byte" sequence exists at four more
sites the scan popup can reach:

| Site | Consequence today |
| --- | --- |
| `file.cc:57-66` `File::loadFromFile()` | A zero-byte local file aborts before its empty vector is returned. This is the first thing `MddResourceRequest::run()` tries, and it also serves local audio and image resources. |
| `xdxf.cc:1150` `XdxfResourceRequest` | Empty image bytes are passed through `&data.front()` to `QImage::fromData` (and the TIFF fallback). Unreachable for empty input until `loadFromFile` is repaired, then reachable. |
| `xdxf.cc:1161` `XdxfResourceRequest` | An empty BMP conversion buffer (`img.save` failure) is copied through `&data.front()`. |
| `xdxf.cc:657` `XdxfArticleRequest` | An article result that appends nothing is copied through `&data.front()`. |

Two nearby sites are deliberately **not** changed: the `article_maker.cc` copies
(`346`, `404`, `418`, `434`, `512`, `752`) copy fixed, non-empty article headers, and
the MDX redirection reads already guard on `data.size()`.

## Goals

- The scan popup never aborts because a resource payload is empty.
- Non-empty payloads are served byte-for-byte as before; rendering of normal
  dictionaries does not change.
- One documented mechanism owns the copy rule, so a new producer cannot reintroduce
  the unguarded idiom unnoticed.
- A regression suite fails on the pre-repair code and passes after it.

## Non-goals

- Changing what `isolateCSS()` keeps or drops (for example re-injecting `@page`
  rules). Producing an empty stylesheet is correct behaviour.
- Changing the distinction between "resource absent" and "resource empty": the
  `hasAnyData`/`dataSize()` contract stays exactly as it is, and an empty payload is
  treated the way an empty resource is treated today.
- Touching unrelated producers or refactoring the request classes.
- Any user-visible message, dialog, or UI change.

## Design

### A single payload-copy helper

A new header-only module owns the rule:

```cpp
// payload.hh
namespace Payload {
/// Replaces `destination` with exactly `size` bytes copied from `bytes`.
/// A zero-byte payload is legal: the vector becomes empty and no byte is
/// dereferenced (an empty vector has no first byte to take).
inline void assign( std::vector< char > & destination,
                    char const * bytes, std::size_t size );
}
```

Contract:

- `size == 0`: `destination` becomes empty; neither `bytes` nor the vector's storage
  is dereferenced, so a null source pointer is acceptable.
- `size > 0`: `destination` holds exactly those bytes, replacing any previous content.
- No exceptions beyond the vector's own allocation failure; no Qt dependency.

It is header-only so the low-level callers (`file.cc`) and the existing test
translation units can include it without a new library or link order.

### Site changes

| Site | Change |
| --- | --- |
| `mdx.cc:938-940` | Replace the resize-and-`front()` copy with `Payload::assign( data, bytes.constData(), bytes.size() )`. This is the reported crash. |
| `file.cc:57-66` | Keep `data.resize( f.tell() )`, but read from the stream only when the vector is non-empty. A zero-byte file yields an empty vector. |
| `xdxf.cc:657` | Replace the copy of `result` with `Payload::assign( data, result.data(), result.size() )`. |
| `xdxf.cc:1150` | Skip the image decode (and TIFF fallback) when `data` is empty, so the pointer is never formed. |
| `xdxf.cc:1161` | Replace the BMP copy with `Payload::assign( data, buffer.constData(), buffer.size() )`. |

`payload.hh` is added to the application project's header list.

### Behaviour when a payload is empty

- **MDD stylesheet**: the request finishes with no data, exactly as an empty resource
  already does; the popup renders the article with GoldenDict's own styling. Nothing
  is displayed differently for dictionaries whose stylesheets are non-empty.
- **Local file**: an empty file loads as an empty vector instead of aborting.
- **XDXF image**: an empty image resource finishes without decoding; it is reported as
  the zero-byte resource it is.
- **XDXF article**: an empty article result is served as empty, as it would have been
  without the abort.

### Alternatives considered

1. **Inline guards at each site only.** Smallest diff, but the reported `mdx.cc` site
   would remain untestable in the existing no-display suites, and four copies of the
   same condition invite a fifth. Rejected.
2. **A safe-accessor or wrapper type for every request payload.** Would make the
   invariant stronger but changes a widely used container and every producer. Out of
   proportion to this defect. Rejected.
3. **Shared helper (chosen).** One rule, one place to test, applied at exactly the
   sites that can see an empty payload, with no change to the request API.

## Error handling

No new exceptions, error strings, retries, or logging. An empty payload is a normal
outcome that the web engine already handles (the resource is simply not served). The
change converts an abort into the same silent empty-resource path the code already
uses for a missing entry.

## Test strategy

A new suite, `tests/tst_resource_payload`, in ordinary (no-display) mode. It links the
production translation units that own the rule and its callers — `dictionary.cc` and
`file.cc` with the same support closure `tst_btree_index` already uses — and pins:

1. `payloadAssign_emptyLeavesVectorEmpty` — the reported crash reduced to one call;
   fails by abort on the pre-repair helper.
2. `payloadAssign_copiesExactBytes` — every byte, no more, no less.
3. `payloadAssign_replacesLongerContent` — a shorter payload leaves no residue.
4. `loadFromFile_zeroByteFileIsEmpty` — fails by abort on the pre-repair `file.cc`.
5. `loadFromFile_readsExactBytes` — the non-empty path is unchanged.
6. `isolateCss_commentOnlyIsEmpty` — documents the trigger that reached the crash.
7. `isolateCss_pageOnlyIsEmpty` — the second legitimate empty case.
8. `isolateCss_prefixesOrdinaryRules` — ordinary stylesheets still get their isolation
   prefix, so the fix does not weaken isolation.

The suite inventory in `tests/tests.pro` is a deliberate exact-count gate ("exactly the
20 required suites"). It becomes 21 with `tst_resource_payload` added to
`GD_REQUIRED_SUITE_NAMES`, and its comment is updated with it. That gate change is
part of this design, not an incidental edit.

### Failing-first evidence

The helper first lands as a behaviour-preserving extraction: the unsafe
`resize`+`front()` body is moved verbatim out of `mdx.cc`, and every call site is
rewired with no intended behaviour change. The new suite then runs against that
extraction and must abort in case 1 (and, once `file.cc` is wired, case 4) with the
same `__glibcxx_assert_fail` signature as the reported coredump. Only then is the
helper's zero-size guard added. The abort is recorded as the failure evidence.

## Risks

- **Suite inventory gate.** Raising the exact count from 20 to 21 is deliberate and
  reviewed; the gate continues to fail on an unregistered suite.
- **Empty vs missing.** An empty stylesheet is still reported as "no data", which is
  today's behaviour for an empty resource. No visual regression is expected for
  dictionaries with non-empty stylesheets; the suite pins that isolation still prefixes
  ordinary rules.
- **Link closure.** `mdx.cc` and `xdxf.cc` are not part of the payload suite's link
  closure, so their edits are compile-proven by the application build, while the
  mechanism they call is behaviour-proven by the suite. The audit verifies both.
- **Header-only helper.** It must stay dependency-free; including `<cstring>`,
  `<cstddef>`, and `<vector>` only.

## Verification

- Configure and build `tests/tests.pro` from the integration worktree and run the full
  21-suite gate; every suite passes.
- Build the application from the integration worktree, which is the compile proof for
  `mdx.cc` and `xdxf.cc`.
- Re-run the new suite's two empty-payload cases against the pre-repair extraction to
  retain the abort evidence.
- If the dictionary that produced the coredump is identified, load that dictionary's
  real bundled stylesheet through the repaired tree before delivery.

## Acceptance criteria

- No unguarded "resize then `front()`" copy remains at the five sites listed above.
- `tst_resource_payload` fails on the pre-repair code and passes on the repaired code.
- The complete suite gate and the application build both pass in the integration
  worktree.
- No change to rendered articles for dictionaries whose resources are non-empty.

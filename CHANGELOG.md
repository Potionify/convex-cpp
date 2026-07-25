# Changelog

Notable changes per release. Versions follow [semantic versioning](https://semver.org/),
with the caveat that 0.x means the API may still move between minor versions.

## [0.1.2] — 2026-07-25

### Changed

- `convex::paginated_query` now splits an oversized page instead of resetting
  pagination. A page's range is pinned by its query journal, so inserts into
  that range make it grow; previously a page that outgrew the server's read
  limits (`pageStatus: "SplitRequired"`) reset the whole session, dropping every
  loaded page and snapping the list back to the first one. It is now split in
  two at the server's `splitCursor`, with both halves bounded by explicit
  `cursor`/`endCursor` and swapped in only once both have loaded, so loaded
  pages stay put. Pages are also split on `SplitRecommended`, or once a page
  grows past twice `initial_num_items`. This matches convex-js's
  `usePaginatedQuery`.
- A page the server reports as `SplitRequired` may be missing part of its range,
  so `paginated_snapshot::results` now stops before it and the status returns to
  `loading_more` (or `loading_first_page`) until a split repairs it. Previously
  such a page was published, which could leave a gap in the middle of the list.
  Also matches `usePaginatedQuery`.
- An incomplete page the server gives no `splitCursor` for is left alone rather
  than reset. Its subscription stays live, so a later result that fits the read
  limits repairs it. A query too heavy to ever return a complete page leaves the
  list loading, with no error — as in convex-js, which has no recovery path for
  that case either.

No public API change: `paginated_query`, `paginated_snapshot`, and
`pagination_status` are unchanged.

## [0.1.1] — 2026-07-14

### Added

- FetchContent consumption snippet in the README.

## [0.1.0] — 2026-07-14

First release. Realtime sync client over the Convex WebSocket protocol (live
query subscriptions, ordered mutations with read-your-writes, actions, auth,
reconnection with jittered backoff), the `convex::paginated_query` helper,
`convex::http_client` for one-shot calls, file storage upload/download, and full
Convex value fidelity including Int64, Bytes, and non-finite floats. The
protocol core is sans-IO; a bundled IXWebSocket transport is optional.

[0.1.2]: https://github.com/Potionify/convex-cpp/compare/v0.1.1...v0.1.2
[0.1.1]: https://github.com/Potionify/convex-cpp/compare/v0.1.0...v0.1.1
[0.1.0]: https://github.com/Potionify/convex-cpp/releases/tag/v0.1.0

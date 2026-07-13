#pragma once

// Client-side pagination helper over convex::client, ported from convex-js's
// usePaginatedQuery hook. Each loaded page is one live server subscription;
// load_more() subscribes the next page at the previous page's continueCursor.
// Page boundaries stay seam-free across updates because the server records
// each page's end cursor in its query journal, which base_client stores and
// resends per subscription.
//
// The paginated query function must take a "paginationOpts" argument
// (paginationOptsValidator) and return a PaginationResult:
//   { page: [...], isDone: bool, continueCursor: string }.
//
// Divergence from convex-js (v1): page splitting is NOT implemented. When
// the server reports pageStatus == "SplitRequired" (a page grew past the
// read limits and may be incomplete), convex-js splits the page in two via
// splitCursor; this helper instead resets pagination, which re-fetches
// correctly-sized pages. "SplitRecommended" is ignored. Revisit if reset
// churn ever matters in practice.
//
// Threading. All callbacks (page updates) arrive through the owning client's
// delivery mechanism — the process_events() pump by default. The on_update
// callback is invoked with an internal recursive mutex held: calling
// load_more()/set_args()/reset()/snapshot() from inside it is safe, but
// destroying the helper from another thread blocks until the callback
// returns (and after the destructor returns, on_update never fires again).
// The helper must not outlive the client it was created from.

#include <convex/client.h>
#include <convex/error.h>
#include <convex/value.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace convex {

/// Where a paginated list stands, mirroring convex-js's PaginationStatus
/// (plus `error`, which convex-js models by throwing).
enum class pagination_status : std::uint8_t {
    /// The only page is the first one and it has no result yet (fresh
    /// helper, or right after a reset).
    loading_first_page,
    /// Every requested page is loaded and the server has more items.
    can_load_more,
    /// More than one page exists and the newest has no result yet.
    loading_more,
    /// The server reached the end of the list.
    exhausted,
    /// A page failed with an error other than InvalidCursor (those reset
    /// pagination instead). See paginated_snapshot::error.
    error,
};

/// convex-js-style name for a status ("LoadingFirstPage", ...).
std::string_view pagination_status_name(pagination_status status);

/// Point-in-time view of a paginated list: every loaded item in order plus
/// the pagination status.
struct paginated_snapshot {
    /// Items from all loaded pages, concatenated in page order. On error,
    /// holds the items from the pages before the failed one.
    std::vector<value> results;
    pagination_status status = pagination_status::loading_first_page;
    /// The failing page's result when status == error.
    std::optional<function_result> error;

    bool is_loading() const {
        return status == pagination_status::loading_first_page ||
               status == pagination_status::loading_more;
    }
};

struct paginated_impl;

/// A growing, live-updating list backed by a paginated Convex query.
///
/// Construction subscribes the first page. Every page is a normal live
/// subscription, so items keep updating (and pages keep their boundaries)
/// until the helper is destroyed. Destroying the helper unsubscribes all
/// pages.
///
/// Pagination resets — dropping every page and re-subscribing a fresh first
/// page with a new cache-buster id — happen when set_args() changes the
/// arguments, when a page fails with InvalidCursor (cursors went stale), or
/// when the server reports a page as SplitRequired (see file comment).
class paginated_query {
public:
    using snapshot_callback = std::function<void(const paginated_snapshot&)>;

    struct options {
        /// Query path, e.g. "messages:listPaginated".
        std::string udf_path;
        /// Arguments for the query, excluding "paginationOpts" (injected by
        /// the helper).
        value_object args;
        /// Page size requested for the first page and after resets.
        std::size_t initial_num_items = 0;
    };

    /// Subscribes the first page. `on_update` fires with a fresh snapshot on
    /// every change (page results, load_more, resets); it may be null if the
    /// caller polls snapshot() instead. Throws std::invalid_argument when
    /// udf_path is empty or initial_num_items is zero.
    paginated_query(client& client, options options, snapshot_callback on_update);

    /// Unsubscribes every page. After this returns, on_update never fires.
    ~paginated_query();

    paginated_query(paginated_query&&) noexcept = default;
    paginated_query& operator=(paginated_query&& other) noexcept;
    paginated_query(const paginated_query&) = delete;
    paginated_query& operator=(const paginated_query&) = delete;

    /// Request the next page of up to `num_items` items. Only acts when the
    /// status is can_load_more (a no-op while loading, after exhaustion, and
    /// on error — mirroring convex-js). Returns true when a page load
    /// actually started.
    bool load_more(std::size_t num_items);

    /// Replace the query arguments. When they differ from the current ones
    /// (compared canonically), pagination resets to a fresh first page;
    /// returns whether that happened.
    bool set_args(value_object args);

    /// Drop every page and start over from a fresh first page.
    void reset();

    /// Current combined results + status.
    paginated_snapshot snapshot() const;

    pagination_status status() const { return snapshot().status; }

private:
    std::shared_ptr<paginated_impl> impl_;
};

}  // namespace convex

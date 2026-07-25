#include <convex/paginated.h>

#include <convex/protocol.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace convex {

namespace {

/// Cache-buster shared with every helper instance in the process, mirroring
/// convex-js's module-global counter. A unique id per pagination session
/// keeps its pages' query journals independent (no deduplication onto an
/// existing subscription whose page may have grown or shrunk) and makes
/// error resets actually re-fetch instead of replaying a cached failure.
std::int64_t next_pagination_id() {
    static std::atomic<std::int64_t> counter{0};
    return ++counter;
}

/// The interesting parts of a PaginationResult value.
struct page_fields {
    const value_array* items = nullptr;
    bool is_done = false;
    std::string continue_cursor;
    /// Where the server suggests this page be cut in two. Absent when the
    /// page is too small to have a split point.
    std::optional<std::string> split_cursor;
    bool split_required = false;
    bool split_recommended = false;
};

std::optional<page_fields> parse_page(const value& v) {
    if (!v.is_object()) return std::nullopt;
    const value_object& o = v.as_object();
    page_fields f;
    const auto page_it = o.find("page");
    const auto done_it = o.find("isDone");
    const auto cursor_it = o.find("continueCursor");
    if (page_it == o.end() || !page_it->second.is_array()) return std::nullopt;
    if (done_it == o.end() || !done_it->second.is_boolean()) return std::nullopt;
    if (cursor_it == o.end() || !cursor_it->second.is_string()) return std::nullopt;
    f.items = &page_it->second.as_array();
    f.is_done = done_it->second.as_boolean();
    f.continue_cursor = cursor_it->second.as_string();
    if (const auto split_it = o.find("splitCursor");
        split_it != o.end() && split_it->second.is_string()) {
        f.split_cursor = split_it->second.as_string();
    }
    if (const auto status_it = o.find("pageStatus");
        status_it != o.end() && status_it->second.is_string()) {
        const std::string& status = status_it->second.as_string();
        f.split_required = status == "SplitRequired";
        f.split_recommended = status == "SplitRecommended";
    }
    return f;
}

/// InvalidCursor means the paginated query was data-dependent and changed
/// underneath us: the cursors in our args/journals no longer match. The fix
/// is always a full reset. Matches convex-js's two detection paths (message
/// substring, and the structured ConvexError payload).
bool is_invalid_cursor(const function_result& r) {
    if (r.ok()) return false;
    if (r.error_message().find("InvalidCursor") != std::string::npos) return true;
    if (const convex_error* e = r.app_error(); e != nullptr && e->data.is_object()) {
        const value_object& data = e->data.as_object();
        const auto sys_it = data.find("isConvexSystemError");
        const auto kind_it = data.find("paginationError");
        return sys_it != data.end() && sys_it->second == value(true) &&
               kind_it != data.end() && kind_it->second == value("InvalidCursor");
    }
    return false;
}

}  // namespace

std::string_view pagination_status_name(pagination_status status) {
    switch (status) {
        case pagination_status::loading_first_page:
            return "LoadingFirstPage";
        case pagination_status::can_load_more:
            return "CanLoadMore";
        case pagination_status::loading_more:
            return "LoadingMore";
        case pagination_status::exhausted:
            return "Exhausted";
        case pagination_status::error:
            return "Error";
    }
    return "Unknown";
}

struct paginated_impl : std::enable_shared_from_this<paginated_impl> {
    /// Identifies a page across reorderings. Page positions shift when a
    /// split swaps one page for two, so callbacks address pages by key.
    using page_key = std::uint64_t;

    // Immutable after construction.
    client* owner;
    std::string udf_path;
    std::size_t initial_num_items;

    // Guarded by `m` (recursive: the on_update callback and the synchronous
    // cached-result delivery inside client::subscribe both re-enter).
    mutable std::recursive_mutex m;
    value_object user_args;
    paginated_query::snapshot_callback on_update;
    std::int64_t pagination_id = 0;
    /// Bumped by every reset; page callbacks created before the bump no-op.
    /// (In pumped mode, callbacks already queued keep firing after their
    /// subscription is dropped, so unsubscribing alone is not enough.)
    std::uint64_t generation = 0;
    bool stopped = false;

    struct page {
        page_key key = 0;
        client::subscription sub;
        std::optional<function_result> result;
        /// Start of the page; absent means the beginning of the query.
        std::optional<std::string> cursor;
        /// End of the page. Only set on the halves of a split, which pin
        /// both ends of their range so the two of them cover exactly what
        /// the page they replace covered.
        std::optional<std::string> end_cursor;
    };

    /// A split in flight: two half-pages loading in parallel while the page
    /// they will replace stays live and visible. Swapping them in only once
    /// both have results keeps the combined list gapless at every moment.
    struct pending_split {
        page_key original = 0;
        page first;
        page second;
    };

    /// Active pages, in result order.
    std::vector<page> pages;
    /// Splits in flight. Their halves are not in `pages` yet.
    std::vector<pending_split> splits;
    page_key next_key = 0;

    paginated_impl(client& c, paginated_query::options&& o,
                   paginated_query::snapshot_callback&& cb)
        : owner(&c),
          udf_path(std::move(o.udf_path)),
          initial_num_items(o.initial_num_items),
          user_args(std::move(o.args)),
          on_update(std::move(cb)),
          pagination_id(next_pagination_id()) {}

    // Requires `m`. Looks up a page by key, in `pages` or in a split's halves.
    page* find_page(page_key key) {
        for (page& p : pages) {
            if (p.key == key) return &p;
        }
        for (pending_split& s : splits) {
            if (s.first.key == key) return &s.first;
            if (s.second.key == key) return &s.second;
        }
        return nullptr;
    }

    // Requires `m`. Index into `splits` of the split `key` is a half of.
    std::optional<std::size_t> split_of_half(page_key key) const {
        for (std::size_t i = 0; i < splits.size(); ++i) {
            if (splits[i].first.key == key || splits[i].second.key == key) return i;
        }
        return std::nullopt;
    }

    // Requires `m`. True while `key` is being replaced by two halves.
    bool is_splitting(page_key key) const {
        return std::any_of(splits.begin(), splits.end(),
                           [key](const pending_split& s) { return s.original == key; });
    }

    // Requires `m`. Subscribes an already-stored page to its page query.
    void subscribe_page(page& p, std::size_t num_items) {
        const std::uint64_t gen = generation;
        const page_key key = p.key;
        value_object opts;
        // numItems and id are float64 on the wire: paginationOptsValidator
        // uses v.number(), which rejects Convex int64 ($integer).
        opts.emplace("numItems", value(static_cast<double>(num_items)));
        opts.emplace("cursor", p.cursor ? value(*p.cursor) : value(nullptr));
        if (p.end_cursor) opts.emplace("endCursor", value(*p.end_cursor));
        opts.emplace("id", value(static_cast<double>(pagination_id)));
        value_object args = user_args;
        args.insert_or_assign("paginationOpts", value(std::move(opts)));
        // subscribe() delivers an already-known result synchronously, so the
        // page callback can run — and reset the session, destroying `p` —
        // before this returns. Everything after it goes through find_page;
        // if the page is gone, the local subscription drops on scope exit.
        client::subscription sub = owner->subscribe(
            udf_path, std::move(args),
            [self = shared_from_this(), gen, key](const function_result& r) {
                self->on_page_result(gen, key, r);
            });
        if (gen != generation) return;
        if (page* current = find_page(key)) current->sub = std::move(sub);
    }

    // Requires `m`. Appends and subscribes one page at the end of the list.
    void add_page(std::size_t num_items, std::optional<std::string> cursor) {
        pages.emplace_back();
        page& p = pages.back();
        p.key = ++next_key;
        p.cursor = std::move(cursor);
        subscribe_page(p, num_items);
    }

    // Requires `m`. Drops every page and starts a fresh session.
    void do_reset() {
        ++generation;
        pagination_id = next_pagination_id();
        splits.clear();
        pages.clear();  // unsubscribes
        add_page(initial_num_items, std::nullopt);
    }

    // Requires `m`. Starts replacing `original` with two halves covering the
    // same range: (cursor, split_cursor] and (split_cursor, continue_cursor].
    void start_split(page_key original, const std::string& split_cursor,
                     const std::string& continue_cursor) {
        const page* orig = find_page(original);
        if (orig == nullptr) return;
        pending_split s;
        s.original = original;
        s.first.key = ++next_key;
        // The first half starts where the original page started — NOT at the
        // beginning of the query. convex-js sent a null cursor here, which
        // duplicated every item before the page (convex-js commit 7ceee3e).
        s.first.cursor = orig->cursor;
        s.first.end_cursor = split_cursor;
        s.second.key = ++next_key;
        s.second.cursor = split_cursor;
        s.second.end_cursor = continue_cursor;
        const page_key first_key = s.first.key;
        const page_key second_key = s.second.key;
        splits.push_back(std::move(s));
        // Re-find between the two: subscribing the first half can re-enter
        // and move the splits vector out from under us.
        if (page* p = find_page(first_key)) subscribe_page(*p, initial_num_items);
        if (page* p = find_page(second_key)) subscribe_page(*p, initial_num_items);
    }

    // Requires `m`. Called when a half of split `index` gets a result.
    void advance_split(std::size_t index) {
        {
            pending_split& s = splits[index];
            // Look for a failure before waiting on the peer. A split with a
            // failed half can never complete, and while it sits here
            // is_splitting blocks every later attempt to repair the page it
            // was splitting — including an update that would have worked.
            if ((s.first.result && !s.first.result->ok()) ||
                (s.second.result && !s.second.result->ok())) {
                // An ordinary error; InvalidCursor resets before we get here.
                // Drop the split and leave the page it was repairing in place.
                // The next update of that page tries again. Retrying from here
                // instead would re-subscribe the same failing query, whose
                // cached error is delivered synchronously, and recurse.
                splits.erase(splits.begin() + static_cast<std::ptrdiff_t>(index));
                return;
            }
            if (!s.first.result || !s.second.result) return;
        }
        pending_split done = std::move(splits[index]);
        splits.erase(splits.begin() + static_cast<std::ptrdiff_t>(index));
        const auto it = std::find_if(pages.begin(), pages.end(), [&](const page& p) {
            return p.key == done.original;
        });
        if (it == pages.end()) return;
        const auto at = it - pages.begin();
        const page_key first_key = done.first.key;
        const page_key second_key = done.second.key;
        *it = std::move(done.first);  // unsubscribes the page being replaced
        pages.insert(pages.begin() + at + 1, std::move(done.second));
        // A half can be oversized in its own right (the range it inherited
        // may still be too big), so keep splitting until it isn't.
        consider_split(first_key);
        consider_split(second_key);
    }

    // Requires `m`. Splits `key` if the server asked for it, or if the page
    // has outgrown twice the requested size. Mirrors convex-js's
    // processPaginatedQuerySplits.
    void consider_split(page_key key) {
        page* p = find_page(key);
        if (p == nullptr || !p->result || !p->result->ok()) return;
        if (is_splitting(key)) return;
        const auto f = parse_page(p->result->get_value());
        if (!f) return;
        // No split point means nothing to do: an incomplete page stays out of
        // the snapshot, and its subscription is live, so a later result that
        // fits the limits repairs it on its own.
        if (!f->split_cursor) return;
        if (f->split_required || f->split_recommended ||
            f->items->size() > initial_num_items * 2) {
            start_split(key, *f->split_cursor, f->continue_cursor);
        }
    }

    void on_page_result(std::uint64_t gen, page_key key, const function_result& r) {
        std::lock_guard lk(m);
        if (stopped || gen != generation) return;
        page* p = find_page(key);
        if (p == nullptr) return;
        p->result = r;
        if (is_invalid_cursor(r)) {
            do_reset();
        } else if (const auto index = split_of_half(key); index) {
            advance_split(*index);
        } else {
            consider_split(key);
        }
        notify();
    }

    // Requires `m`.
    paginated_snapshot compute_snapshot() const {
        paginated_snapshot snap;
        bool all_loaded = true;
        bool last_is_done = false;
        for (const page& p : pages) {
            if (!p.result) {
                all_loaded = false;
                break;
            }
            if (!p.result->ok()) {
                snap.status = pagination_status::error;
                snap.error = *p.result;
                return snap;
            }
            const auto f = parse_page(p.result->get_value());
            if (!f) {
                snap.status = pagination_status::error;
                snap.error = function_result::error(
                    "convex: paginated query \"" + udf_path +
                    "\" returned a value that is not a PaginationResult");
                return snap;
            }
            if (f->split_required) {
                // The server could not read this page in full, so its items
                // may be missing part of the range. Stop before it rather
                // than publish a list with a hole in it — the split (or the
                // reset) that repairs it is already under way. This is what
                // convex-js's usePaginatedQuery does.
                all_loaded = false;
                break;
            }
            snap.results.insert(snap.results.end(), f->items->begin(), f->items->end());
            last_is_done = f->is_done;
        }
        if (!all_loaded) {
            snap.status = snap.results.empty() ? pagination_status::loading_first_page
                                               : pagination_status::loading_more;
        } else {
            snap.status = last_is_done ? pagination_status::exhausted
                                       : pagination_status::can_load_more;
        }
        return snap;
    }

    // Requires `m`. Copies the callback first so on_update can safely be
    // cleared (helper destroyed) from inside its own invocation.
    void notify() {
        if (!on_update) return;
        const auto cb = on_update;
        cb(compute_snapshot());
    }

    void stop() {
        std::lock_guard lk(m);
        stopped = true;
        on_update = nullptr;
        splits.clear();
        pages.clear();
    }
};

paginated_query::paginated_query(client& client, options options, snapshot_callback on_update) {
    if (options.udf_path.empty()) {
        throw std::invalid_argument("convex: paginated_query requires a udf_path");
    }
    if (options.initial_num_items == 0) {
        throw std::invalid_argument("convex: paginated_query initial_num_items must be > 0");
    }
    impl_ = std::make_shared<paginated_impl>(client, std::move(options), std::move(on_update));
    std::lock_guard lk(impl_->m);
    impl_->add_page(impl_->initial_num_items, std::nullopt);
}

paginated_query::~paginated_query() {
    if (impl_) impl_->stop();
}

paginated_query& paginated_query::operator=(paginated_query&& other) noexcept {
    if (this != &other) {
        if (impl_) impl_->stop();
        impl_ = std::move(other.impl_);
    }
    return *this;
}

bool paginated_query::load_more(std::size_t num_items) {
    if (!impl_ || num_items == 0) return false;
    std::lock_guard lk(impl_->m);
    if (impl_->stopped) return false;
    if (impl_->compute_snapshot().status != pagination_status::can_load_more) return false;
    // can_load_more implies every page is loaded, ok, and well-formed.
    const auto last = parse_page(impl_->pages.back().result->get_value());
    impl_->add_page(num_items, last->continue_cursor);
    impl_->notify();
    return true;
}

bool paginated_query::set_args(value_object args) {
    if (!impl_) return false;
    std::lock_guard lk(impl_->m);
    if (impl_->stopped) return false;
    if (serialize_args(args) == serialize_args(impl_->user_args)) return false;
    impl_->user_args = std::move(args);
    impl_->do_reset();
    impl_->notify();
    return true;
}

void paginated_query::reset() {
    if (!impl_) return;
    std::lock_guard lk(impl_->m);
    if (impl_->stopped) return;
    impl_->do_reset();
    impl_->notify();
}

paginated_snapshot paginated_query::snapshot() const {
    if (!impl_) return {};
    std::lock_guard lk(impl_->m);
    return impl_->compute_snapshot();
}

}  // namespace convex

#include <convex/paginated.h>

#include <convex/protocol.h>

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
    bool split_required = false;
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
    if (const auto status_it = o.find("pageStatus"); status_it != o.end()) {
        f.split_required =
            status_it->second.is_string() && status_it->second.as_string() == "SplitRequired";
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
        client::subscription sub;
        std::optional<function_result> result;
    };
    std::vector<page> pages;

    paginated_impl(client& c, paginated_query::options&& o,
                   paginated_query::snapshot_callback&& cb)
        : owner(&c),
          udf_path(std::move(o.udf_path)),
          initial_num_items(o.initial_num_items),
          user_args(std::move(o.args)),
          on_update(std::move(cb)),
          pagination_id(next_pagination_id()) {}

    // Requires `m`. Appends and subscribes one page.
    void add_page(std::size_t num_items, std::optional<std::string> cursor) {
        const std::uint64_t gen = generation;
        const std::size_t index = pages.size();
        pages.emplace_back();
        value_object opts;
        // numItems and id are float64 on the wire: paginationOptsValidator
        // uses v.number(), which rejects Convex int64 ($integer).
        opts.emplace("numItems", value(static_cast<double>(num_items)));
        opts.emplace("cursor", cursor ? value(std::move(*cursor)) : value(nullptr));
        opts.emplace("id", value(static_cast<double>(pagination_id)));
        value_object args = user_args;
        args.insert_or_assign("paginationOpts", value(std::move(opts)));
        pages[index].sub = owner->subscribe(
            udf_path, std::move(args),
            [self = shared_from_this(), gen, index](const function_result& r) {
                self->on_page_result(gen, index, r);
            });
    }

    // Requires `m`. Drops every page and starts a fresh session.
    void do_reset() {
        ++generation;
        pagination_id = next_pagination_id();
        pages.clear();  // unsubscribes
        add_page(initial_num_items, std::nullopt);
    }

    void on_page_result(std::uint64_t gen, std::size_t index, const function_result& r) {
        std::lock_guard lk(m);
        if (stopped || gen != generation) return;
        pages[index].result = r;
        if (is_invalid_cursor(r)) {
            do_reset();
        } else if (r.ok()) {
            if (const auto f = parse_page(r.get_value()); f && f->split_required) {
                // The page outgrew the server's read limits and may be
                // incomplete. convex-js splits it; we reset (see header).
                do_reset();
            }
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
            snap.results.insert(snap.results.end(), f->items->begin(), f->items->end());
            last_is_done = f->is_done;
        }
        if (!all_loaded) {
            snap.status = (pages.size() == 1) ? pagination_status::loading_first_page
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

#include <convex/base_client.h>

#include <algorithm>

namespace convex {

namespace {

std::string query_token(const std::string& udf_path, const std::string& args_json) {
    // Canonical because both components are canonical (sorted object keys).
    return udf_path + "|" + args_json;
}

}  // namespace

// ---------------------------------------------------------------- local ops

subscriber_id base_client::subscribe(std::string_view udf_path, value_object args) {
    std::string canonical = canonicalize_udf_path(udf_path);
    std::string args_json = serialize_args(args);
    const std::string token = query_token(canonical, args_json);

    if (const auto it = query_set_.find(token); it != query_set_.end()) {
        ++it->second.num_subscribers;
        return subscriber_id{it->second.id, next_subscription_index_++};
    }

    local_query q;
    q.id = next_query_id_++;
    q.udf_path = std::move(canonical);
    q.args_json = std::move(args_json);
    q.num_subscribers = 1;

    query_add add;
    add.id = q.id;
    add.udf_path = q.udf_path;
    add.args_json = q.args_json;

    id_to_token_.emplace(q.id, token);
    const query_id qid = q.id;
    query_set_.emplace(token, std::move(q));

    modify_query_set_message m;
    m.base_version = query_set_version_;
    m.new_version = ++query_set_version_;
    m.modifications.push_back(std::move(add));
    outgoing_.push_back(std::move(m));

    return subscriber_id{qid, next_subscription_index_++};
}

void base_client::unsubscribe(const subscriber_id& id) {
    const auto token_it = id_to_token_.find(id.query);
    if (token_it == id_to_token_.end()) return;
    const auto query_it = query_set_.find(token_it->second);
    if (query_it == query_set_.end()) return;

    if (--query_it->second.num_subscribers > 0) return;

    query_set_.erase(query_it);
    id_to_token_.erase(token_it);
    remote_results_.erase(id.query);
    outstanding_queries_.erase(id.query);

    modify_query_set_message m;
    m.base_version = query_set_version_;
    m.new_version = ++query_set_version_;
    m.modifications.push_back(query_remove{id.query});
    outgoing_.push_back(std::move(m));
}

request_id base_client::mutation(std::string_view udf_path, value_object args) {
    mutation_request_message m;
    m.id = next_request_id_++;
    m.udf_path = canonicalize_udf_path(udf_path);
    m.args_json = serialize_args(args);

    pending_request req;
    req.typ = pending_request::kind::mutation;
    req.original_message = m;
    ongoing_requests_.emplace(m.id, std::move(req));

    const request_id rid = m.id;
    outgoing_.push_back(std::move(m));
    return rid;
}

request_id base_client::action(std::string_view udf_path, value_object args) {
    action_request_message a;
    a.id = next_request_id_++;
    a.udf_path = canonicalize_udf_path(udf_path);
    a.args_json = serialize_args(args);

    pending_request req;
    req.typ = pending_request::kind::action;
    req.original_message = a;
    ongoing_requests_.emplace(a.id, std::move(req));

    const request_id rid = a.id;
    outgoing_.push_back(std::move(a));
    return rid;
}

void base_client::set_auth(auth_token token) {
    if (token.type == auth_token::kind::none) {
        auth_.reset();
    } else {
        auth_ = token;
    }
    authenticate_message m;
    m.base_version = identity_version_++;
    m.token = std::move(token);
    outgoing_.push_back(std::move(m));
}

void base_client::enqueue_authenticate() {
    authenticate_message m;
    m.base_version = identity_version_++;
    m.token = *auth_;
    outgoing_.push_back(std::move(m));
}

// ---------------------------------------------------------------- receive

void base_client::observe_timestamp(timestamp ts) {
    if (!max_observed_timestamp_ || ts > *max_observed_timestamp_) {
        max_observed_timestamp_ = ts;
    }
}

void base_client::flush_completed(timestamp watermark,
                                  std::vector<std::pair<request_id, function_result>>& out) {
    for (auto it = ongoing_requests_.begin(); it != ongoing_requests_.end();) {
        auto& [rid, req] = *it;
        if (req.completed && req.ts && *req.ts <= watermark) {
            out.emplace_back(rid, std::move(*req.result));
            it = ongoing_requests_.erase(it);
        } else {
            ++it;
        }
    }
}

std::string base_client::udf_path_for(query_id id) const {
    const auto token_it = id_to_token_.find(id);
    if (token_it == id_to_token_.end()) return {};
    const auto query_it = query_set_.find(token_it->second);
    return query_it == query_set_.end() ? std::string{} : query_it->second.udf_path;
}

void base_client::apply_state_modification(const state_modification& mod, receive_result& out) {
    if (const auto* updated = std::get_if<query_updated>(&mod)) {
        if (!updated->log_lines.empty()) {
            out.log_entries.push_back(log_entry{log_entry::source_kind::query,
                                                udf_path_for(updated->id), updated->log_lines});
        }
        remote_results_.insert_or_assign(updated->id,
                                         function_result::success(updated->result));
        outstanding_queries_.erase(updated->id);
        if (const auto token_it = id_to_token_.find(updated->id); token_it != id_to_token_.end()) {
            query_set_.at(token_it->second).journal = updated->journal;
        }
    } else if (const auto* failed = std::get_if<query_failed>(&mod)) {
        if (!failed->log_lines.empty()) {
            out.log_entries.push_back(log_entry{log_entry::source_kind::query,
                                                udf_path_for(failed->id), failed->log_lines});
        }
        function_result r =
            failed->error_data
                ? function_result::error(convex_error{failed->error_message, *failed->error_data})
                : function_result::error(failed->error_message);
        remote_results_.insert_or_assign(failed->id, std::move(r));
        outstanding_queries_.erase(failed->id);
        if (const auto token_it = id_to_token_.find(failed->id); token_it != id_to_token_.end()) {
            query_set_.at(token_it->second).journal = failed->journal;
        }
    } else if (const auto* removed = std::get_if<query_removed>(&mod)) {
        remote_results_.erase(removed->id);
        outstanding_queries_.erase(removed->id);
    }
}

base_client::receive_result base_client::handle_transition(const transition_message& t) {
    receive_result out;
    if (t.start_version != remote_version_) {
        out.reconnect_reason = "StartVersionMismatch";
        return out;
    }
    for (const state_modification& mod : t.modifications) {
        apply_state_modification(mod, out);
        std::visit([&out](const auto& m) { out.changed_queries.push_back(m.id); }, mod);
    }
    remote_version_ = t.end_version;
    observe_timestamp(t.end_version.ts);
    flush_completed(t.end_version.ts, out.completed_requests);
    out.state_changed = true;
    return out;
}

base_client::receive_result base_client::receive_message(const server_message& message) {
    receive_result out;

    if (const auto* t = std::get_if<transition_message>(&message)) {
        return handle_transition(*t);
    }

    if (const auto* m = std::get_if<mutation_response_message>(&message)) {
        const auto it = ongoing_requests_.find(m->id);
        if (it == ongoing_requests_.end()) return out;  // duplicate/stale response
        if (!m->log_lines.empty()) {
            const auto* req = std::get_if<mutation_request_message>(&it->second.original_message);
            out.log_entries.push_back(log_entry{log_entry::source_kind::mutation,
                                                req ? req->udf_path : std::string{},
                                                m->log_lines});
        }
        if (m->result.ok()) {
            // Hold the result until a Transition advances past its timestamp,
            // so the mutation's effects are visible before the caller learns
            // it completed (read-your-writes).
            it->second.completed = true;
            it->second.ts = m->ts;
            it->second.result = m->result;
            if (m->ts) {
                observe_timestamp(*m->ts);
                // The watermark may already cover it (e.g. a transition
                // arrived first, or ts predates the current version).
                flush_completed(remote_version_.ts, out.completed_requests);
            } else {
                // A success without ts cannot be ordered; deliver directly.
                out.completed_requests.emplace_back(m->id, std::move(*it->second.result));
                ongoing_requests_.erase(it);
            }
        } else {
            out.completed_requests.emplace_back(m->id, m->result);
            ongoing_requests_.erase(it);
        }
        return out;
    }

    if (const auto* a = std::get_if<action_response_message>(&message)) {
        const auto it = ongoing_requests_.find(a->id);
        if (it == ongoing_requests_.end()) return out;
        if (!a->log_lines.empty()) {
            const auto* req = std::get_if<action_request_message>(&it->second.original_message);
            out.log_entries.push_back(log_entry{log_entry::source_kind::action,
                                                req ? req->udf_path : std::string{},
                                                a->log_lines});
        }
        out.completed_requests.emplace_back(a->id, a->result);
        ongoing_requests_.erase(it);
        return out;
    }

    if (const auto* e = std::get_if<auth_error_message>(&message)) {
        out.reconnect_reason = "AuthError: " + e->error;
        out.auth_error = true;
        out.auth_update_attempted = e->auth_update_attempted;
        return out;
    }

    if (const auto* f = std::get_if<fatal_error_message>(&message)) {
        out.reconnect_reason = "FatalError: " + f->error;
        return out;
    }

    if (std::get_if<transition_chunk_message>(&message) != nullptr) {
        // Chunks belong to the transport layer (transition_chunk_assembler);
        // one reaching the state machine means the caller skipped assembly.
        out.reconnect_reason = "ProtocolError: unassembled TransitionChunk";
        return out;
    }

    // ping_message: server traffic already proves liveness; nothing to do.
    return out;
}

// ---------------------------------------------------------------- outgoing

std::optional<client_message> base_client::pop_next_message() {
    if (outgoing_.empty()) return std::nullopt;
    client_message m = std::move(outgoing_.front());
    outgoing_.pop_front();
    return m;
}

// ---------------------------------------------------------------- restart

std::vector<std::pair<request_id, function_result>> base_client::restart(
    std::optional<auth_token> refreshed_auth) {
    // Actions still sitting in the outgoing queue were never transmitted, so
    // they are safe to resend; only actions already on the wire are at risk
    // of double execution.
    std::set<request_id> unsent_actions;
    for (const client_message& m : outgoing_) {
        if (const auto* a = std::get_if<action_request_message>(&m)) {
            unsent_actions.insert(a->id);
        }
    }

    // Stale queued messages carry stale version numbers; drop them all.
    outgoing_.clear();

    // Versions restart from zero on every connection.
    identity_version_ = 0;
    query_set_version_ = 0;
    remote_version_ = state_version{};
    remote_results_.clear();

    if (refreshed_auth) {
        if (refreshed_auth->type == auth_token::kind::none) {
            auth_.reset();
        } else {
            auth_ = std::move(*refreshed_auth);
        }
    }
    if (auth_) enqueue_authenticate();

    // One ModifyQuerySet (0 -> 1) restoring every live query with its journal.
    outstanding_queries_.clear();
    if (!query_set_.empty()) {
        modify_query_set_message m;
        m.base_version = query_set_version_;
        m.new_version = ++query_set_version_;
        for (const auto& [token, q] : query_set_) {
            query_add add;
            add.id = q.id;
            add.udf_path = q.udf_path;
            add.args_json = q.args_json;
            add.journal = q.journal;
            m.modifications.push_back(std::move(add));
            outstanding_queries_.insert(q.id);
        }
        outgoing_.push_back(std::move(m));
    }

    // Mutations are safe to resend (the server deduplicates on session +
    // request id) and must go out in original order. Actions are not
    // idempotent: fail them.
    std::vector<std::pair<request_id, function_result>> failed_actions;
    for (auto it = ongoing_requests_.begin(); it != ongoing_requests_.end();) {
        if (it->second.typ == pending_request::kind::action &&
            !unsent_actions.contains(it->first)) {
            failed_actions.emplace_back(
                it->first,
                function_result::error("Connection lost while action was in flight"));
            it = ongoing_requests_.erase(it);
        } else {
            ++it;
        }
    }
    for (const auto& [rid, req] : ongoing_requests_) {  // std::map: ascending id order
        outgoing_.push_back(req.original_message);
    }
    return failed_actions;
}

connect_message base_client::make_connect_message(std::string last_close_reason,
                                                  std::optional<std::uint64_t> client_ts) {
    connect_message m;
    m.session_id = session_id_;
    m.connection_count = connection_count_++;
    m.last_close_reason = std::move(last_close_reason);
    m.max_observed_timestamp = max_observed_timestamp_;
    m.client_ts = client_ts;
    return m;
}

const function_result* base_client::latest_result(query_id id) const {
    const auto it = remote_results_.find(id);
    return it == remote_results_.end() ? nullptr : &it->second;
}

std::size_t base_client::inflight_mutations() const {
    return static_cast<std::size_t>(
        std::count_if(ongoing_requests_.begin(), ongoing_requests_.end(), [](const auto& kv) {
            return kv.second.typ == pending_request::kind::mutation;
        }));
}

std::size_t base_client::inflight_actions() const {
    return static_cast<std::size_t>(
        std::count_if(ongoing_requests_.begin(), ongoing_requests_.end(), [](const auto& kv) {
            return kv.second.typ == pending_request::kind::action;
        }));
}

}  // namespace convex

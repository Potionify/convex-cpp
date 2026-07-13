#include <convex/protocol.h>

#include <random>

#include "detail/wire_json.h"

namespace convex {

namespace {

using nlohmann::json;
using detail::le_base64_to_uint64;
using detail::uint64_to_le_base64;

json parse_or_throw(std::string_view text, const char* what) {
    try {
        return json::parse(text);
    } catch (const json::exception& e) {
        throw protocol_error(std::string(what) + ": invalid JSON: " + e.what());
    }
}

// Args are stored pre-serialized (canonical, sorted keys); embed them as a
// parsed DOM so the enclosing message serializes as one document.
json args_dom(const std::string& args_json) {
    return parse_or_throw(args_json, "args");
}

const json& require(const json& j, const char* field, const char* message_type) {
    const auto it = j.find(field);
    if (it == j.end()) {
        throw protocol_error(std::string(message_type) + ": missing field '" + field + "'");
    }
    return *it;
}

std::uint32_t require_u32(const json& j, const char* field, const char* message_type) {
    const json& f = require(j, field, message_type);
    if (!f.is_number_unsigned() && !f.is_number_integer()) {
        throw protocol_error(std::string(message_type) + ": field '" + field +
                             "' is not an integer");
    }
    return f.get<std::uint32_t>();
}

std::string require_string(const json& j, const char* field, const char* message_type) {
    const json& f = require(j, field, message_type);
    if (!f.is_string()) {
        throw protocol_error(std::string(message_type) + ": field '" + field +
                             "' is not a string");
    }
    return f.get<std::string>();
}

timestamp decode_timestamp(const json& f, const char* message_type) {
    if (!f.is_string()) {
        throw protocol_error(std::string(message_type) + ": timestamp is not a string");
    }
    try {
        return le_base64_to_uint64(f.get_ref<const std::string&>());
    } catch (const codec_error& e) {
        throw protocol_error(std::string(message_type) + ": bad timestamp: " + e.what());
    }
}

std::vector<std::string> decode_log_lines(const json& j, const char* message_type) {
    std::vector<std::string> lines;
    const auto it = j.find("logLines");
    if (it == j.end()) return lines;
    if (!it->is_array()) {
        throw protocol_error(std::string(message_type) + ": logLines is not an array");
    }
    for (const auto& line : *it) {
        if (line.is_string()) lines.push_back(line.get<std::string>());
    }
    return lines;
}

std::optional<std::string> optional_journal(const json& j) {
    const auto it = j.find("journal");
    if (it == j.end() || it->is_null()) return std::nullopt;
    if (!it->is_string()) throw protocol_error("journal is not a string");
    return it->get<std::string>();
}

state_version decode_state_version(const json& j, const char* message_type) {
    state_version v;
    v.query_set = require_u32(j, "querySet", message_type);
    v.identity = require_u32(j, "identity", message_type);
    v.ts = decode_timestamp(require(j, "ts", message_type), message_type);
    return v;
}

}  // namespace

std::string canonicalize_udf_path(std::string_view path) {
    std::string module_path;
    std::string function_name;
    const auto colon = path.find(':');
    if (colon == std::string_view::npos) {
        module_path = std::string(path);
        function_name = "default";
    } else {
        module_path = std::string(path.substr(0, colon));
        function_name = std::string(path.substr(colon + 1));
    }
    if (module_path.ends_with(".js")) {
        module_path.erase(module_path.size() - 3);
    }
    return module_path + ":" + function_name;
}

std::string serialize_args(const value_object& args) {
    return "[" + to_wire_json(value(args)) + "]";
}

// ------------------------------------------------------------ encoding

namespace {

json encode_auth(const authenticate_message& m) {
    json j{{"type", "Authenticate"}, {"baseVersion", m.base_version}};
    switch (m.token.type) {
        case auth_token::kind::none:
            j["tokenType"] = "None";
            break;
        case auth_token::kind::user:
            j["tokenType"] = "User";
            j["value"] = m.token.value;
            break;
        case auth_token::kind::admin:
            j["tokenType"] = "Admin";
            j["value"] = m.token.value;
            if (m.token.acting_as) {
                j["impersonating"] = detail::value_to_json(value(*m.token.acting_as));
            }
            break;
    }
    return j;
}

struct client_encoder {
    json operator()(const connect_message& m) const {
        json j{{"type", "Connect"},
               {"sessionId", m.session_id},
               {"connectionCount", m.connection_count},
               {"lastCloseReason", m.last_close_reason}};
        if (m.max_observed_timestamp) {
            j["maxObservedTimestamp"] = uint64_to_le_base64(*m.max_observed_timestamp);
        }
        if (m.client_ts) j["clientTs"] = *m.client_ts;
        return j;
    }

    json operator()(const authenticate_message& m) const { return encode_auth(m); }

    json operator()(const modify_query_set_message& m) const {
        json mods = json::array();
        for (const auto& modification : m.modifications) {
            std::visit(
                [&mods](const auto& mod) {
                    using T = std::decay_t<decltype(mod)>;
                    if constexpr (std::is_same_v<T, query_add>) {
                        json a{{"type", "Add"},
                               {"queryId", mod.id},
                               {"udfPath", mod.udf_path},
                               {"args", args_dom(mod.args_json)}};
                        if (mod.journal) a["journal"] = *mod.journal;
                        if (mod.component_path) a["componentPath"] = *mod.component_path;
                        mods.push_back(std::move(a));
                    } else {
                        mods.push_back(json{{"type", "Remove"}, {"queryId", mod.id}});
                    }
                },
                modification);
        }
        return json{{"type", "ModifyQuerySet"},
                    {"baseVersion", m.base_version},
                    {"newVersion", m.new_version},
                    {"modifications", std::move(mods)}};
    }

    json operator()(const mutation_request_message& m) const {
        json j{{"type", "Mutation"},
               {"requestId", m.id},
               {"udfPath", m.udf_path},
               {"args", args_dom(m.args_json)}};
        if (m.component_path) j["componentPath"] = *m.component_path;
        return j;
    }

    json operator()(const action_request_message& m) const {
        json j{{"type", "Action"},
               {"requestId", m.id},
               {"udfPath", m.udf_path},
               {"args", args_dom(m.args_json)}};
        if (m.component_path) j["componentPath"] = *m.component_path;
        return j;
    }
};

}  // namespace

std::string encode_client_message(const client_message& message) {
    return std::visit(client_encoder{}, message).dump();
}

// ------------------------------------------------------------ decoding

namespace {

transition_message decode_transition(const json& j) {
    transition_message t;
    t.start_version = decode_state_version(require(j, "startVersion", "Transition"), "Transition");
    t.end_version = decode_state_version(require(j, "endVersion", "Transition"), "Transition");
    const json& mods = require(j, "modifications", "Transition");
    if (!mods.is_array()) throw protocol_error("Transition: modifications is not an array");
    for (const json& mod : mods) {
        const std::string mod_type = require_string(mod, "type", "Transition modification");
        if (mod_type == "QueryUpdated") {
            query_updated u;
            u.id = require_u32(mod, "queryId", "QueryUpdated");
            try {
                u.result = detail::json_to_value(require(mod, "value", "QueryUpdated"));
            } catch (const codec_error& e) {
                throw protocol_error(std::string("QueryUpdated: bad value: ") + e.what());
            }
            u.log_lines = decode_log_lines(mod, "QueryUpdated");
            u.journal = optional_journal(mod);
            t.modifications.push_back(std::move(u));
        } else if (mod_type == "QueryFailed") {
            query_failed f;
            f.id = require_u32(mod, "queryId", "QueryFailed");
            f.error_message = require_string(mod, "errorMessage", "QueryFailed");
            const auto data_it = mod.find("errorData");
            if (data_it != mod.end() && !data_it->is_null()) {
                f.error_data = detail::json_to_value(*data_it);
            }
            f.log_lines = decode_log_lines(mod, "QueryFailed");
            f.journal = optional_journal(mod);
            t.modifications.push_back(std::move(f));
        } else if (mod_type == "QueryRemoved") {
            t.modifications.push_back(
                query_removed{require_u32(mod, "queryId", "QueryRemoved")});
        } else {
            throw protocol_error("Transition: unknown modification type '" + mod_type + "'");
        }
    }
    return t;
}

function_result decode_function_result(const json& j, const char* message_type) {
    const json& success = require(j, "success", message_type);
    if (!success.is_boolean()) {
        throw protocol_error(std::string(message_type) + ": success is not a boolean");
    }
    const json& result = require(j, "result", message_type);
    if (success.get<bool>()) {
        try {
            return function_result::success(detail::json_to_value(result));
        } catch (const codec_error& e) {
            throw protocol_error(std::string(message_type) + ": bad result value: " + e.what());
        }
    }
    if (!result.is_string()) {
        throw protocol_error(std::string(message_type) +
                             ": failure result is not an error string");
    }
    std::string message = result.get<std::string>();
    const auto data_it = j.find("errorData");
    if (data_it != j.end() && !data_it->is_null()) {
        return function_result::error(
            convex_error{std::move(message), detail::json_to_value(*data_it)});
    }
    return function_result::error(std::move(message));
}

}  // namespace

server_message decode_server_message(std::string_view json_text) {
    const json j = parse_or_throw(json_text, "server message");
    if (!j.is_object()) throw protocol_error("server message is not an object");
    const std::string type = require_string(j, "type", "server message");

    if (type == "Transition") return decode_transition(j);

    if (type == "MutationResponse") {
        mutation_response_message m;
        m.id = require_u32(j, "requestId", "MutationResponse");
        m.result = decode_function_result(j, "MutationResponse");
        const auto ts_it = j.find("ts");
        if (ts_it != j.end() && !ts_it->is_null()) {
            m.ts = decode_timestamp(*ts_it, "MutationResponse");
        }
        m.log_lines = decode_log_lines(j, "MutationResponse");
        return m;
    }

    if (type == "ActionResponse") {
        action_response_message a;
        a.id = require_u32(j, "requestId", "ActionResponse");
        a.result = decode_function_result(j, "ActionResponse");
        a.log_lines = decode_log_lines(j, "ActionResponse");
        return a;
    }

    if (type == "AuthError") {
        auth_error_message e;
        e.error = require_string(j, "error", "AuthError");
        const auto base_it = j.find("baseVersion");
        if (base_it != j.end() && base_it->is_number()) {
            e.base_version = base_it->get<std::uint32_t>();
        }
        const auto attempted_it = j.find("authUpdateAttempted");
        if (attempted_it != j.end() && attempted_it->is_boolean()) {
            e.auth_update_attempted = attempted_it->get<bool>();
        }
        return e;
    }

    if (type == "FatalError") {
        return fatal_error_message{require_string(j, "error", "FatalError")};
    }

    if (type == "Ping") return ping_message{};

    if (type == "TransitionChunk") {
        transition_chunk_message c;
        c.chunk = require_string(j, "chunk", "TransitionChunk");
        c.part_number = require_u32(j, "partNumber", "TransitionChunk");
        c.total_parts = require_u32(j, "totalParts", "TransitionChunk");
        c.transition_id = require_string(j, "transitionId", "TransitionChunk");
        return c;
    }

    throw protocol_error("unsupported server message type '" + type + "'");
}

std::optional<transition_message> transition_chunk_assembler::feed(
    const transition_chunk_message& chunk) {
    if (chunk.total_parts == 0 || chunk.part_number >= chunk.total_parts ||
        (buffering() && (total_parts_ != chunk.total_parts ||
                         transition_id_ != chunk.transition_id))) {
        abandon();
        throw protocol_error("TransitionChunk: inconsistent chunk parameters");
    }
    if (chunk.part_number != received_parts_) {
        abandon();
        throw protocol_error("TransitionChunk: part " + std::to_string(chunk.part_number) +
                             " received out of order (expected " +
                             std::to_string(received_parts_) + ")");
    }
    if (!buffering()) {
        total_parts_ = chunk.total_parts;
        transition_id_ = chunk.transition_id;
    }
    data_ += chunk.chunk;
    ++received_parts_;
    if (received_parts_ < total_parts_) return std::nullopt;

    const std::string full = std::move(data_);
    abandon();
    server_message assembled = decode_server_message(full);  // may throw
    auto* t = std::get_if<transition_message>(&assembled);
    if (t == nullptr) {
        throw protocol_error("TransitionChunk: assembled message is not a Transition");
    }
    return std::move(*t);
}

void transition_chunk_assembler::abandon() {
    data_.clear();
    received_parts_ = 0;
    total_parts_ = 0;
    transition_id_.clear();
}

std::string generate_session_id() {
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    std::uint64_t hi = rng();
    std::uint64_t lo = rng();
    // Stamp UUID v4 version and variant bits.
    hi = (hi & 0xffffffffffff0fffULL) | 0x0000000000004000ULL;
    lo = (lo & 0x3fffffffffffffffULL) | 0x8000000000000000ULL;

    static constexpr char hex[] = "0123456789abcdef";
    std::string out;
    out.reserve(36);
    const auto emit = [&out](std::uint64_t v, int nibbles) {
        for (int i = nibbles - 1; i >= 0; --i) out.push_back(hex[(v >> (i * 4)) & 0xf]);
    };
    emit(hi >> 32, 8);
    out.push_back('-');
    emit((hi >> 16) & 0xffff, 4);
    out.push_back('-');
    emit(hi & 0xffff, 4);
    out.push_back('-');
    emit(lo >> 48, 4);
    out.push_back('-');
    emit(lo & 0xffffffffffffULL, 12);
    return out;
}

}  // namespace convex

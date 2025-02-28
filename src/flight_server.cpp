/**
 * @file flight_server.cpp
 * @author Ashot Vardanian
 * @date 2022-07-18
 *
 * @brief An server implementing Apache Arrow Flight RPC protocol.
 *
 * Links:
 * https://arrow.apache.org/cookbook/cpp/flight.html
 */

#include <mutex>
#include <fstream>    // `std::ifstream`
#include <charconv>   // `std::from_chars`
#include <chrono>     // `std::time_point`
#include <cstdio>     // `std::printf`
#include <iostream>   // `std::cerr`
#include <filesystem> // Enumerating and creating directories
#include <unordered_map>
#include <unordered_set>

#include <arrow/flight/server.h> // RPC Server Implementation
#include <clipp.h>               // Command Line Interface

#include "ustore/cpp/db.hpp"
#include "ustore/cpp/types.hpp" // `hash_combine`

#include "helpers/arrow.hpp"
#include "ustore/arrow.h"

using namespace unum::ustore;
using namespace unum;

namespace stdfs = std::filesystem;

using sys_clock_t = std::chrono::system_clock;
using sys_time_t = std::chrono::time_point<sys_clock_t>;

inline static arf::ActionType const kActionColOpen {kFlightColCreate, "Find a collection descriptor by name."};
inline static arf::ActionType const kActionColDrop {kFlightColDrop, "Delete a named collection."};
inline static arf::ActionType const kActionSnapOpen {kFlightSnapCreate, "Find a snapshot descriptor by name."};
inline static arf::ActionType const kActionSnapDrop {kFlightSnapDrop, "Delete a named snapshot."};
inline static arf::ActionType const kActionTxnBegin {kFlightTxnBegin, "Starts an ACID transaction and returns its ID."};
inline static arf::ActionType const kActionTxnCommit {kFlightTxnCommit, "Commit a previously started transaction."};

/**
 * @brief Searches for a "value" among key-value pairs passed in URI after path.
 * @param query_params  Must begin with "?" or "/".
 * @param param_name    The name of the URI parameter to match.
 */
std::optional<std::string_view> param_value(std::string_view query_params, std::string_view param_name) {

    char const* key_begin = query_params.begin();
    do {
        key_begin = std::search(key_begin, query_params.end(), param_name.begin(), param_name.end());
        if (key_begin == query_params.end())
            return std::nullopt;
        bool is_suffix = key_begin + param_name.size() == query_params.end();
        if (is_suffix)
            return std::string_view {};

        // Check if we have matched a part of bigger key.
        // In that case skip to next starting point.
        auto prev_character = *(key_begin - 1);
        if (prev_character != '?' && prev_character != '&' && prev_character != '/') {
            key_begin += 1;
            continue;
        }

        auto next_character = key_begin[param_name.size()];
        if (next_character == '&')
            return std::string_view {};

        if (next_character == '=') {
            auto value_begin = key_begin + param_name.size() + 1;
            auto value_end = std::find(value_begin, query_params.end(), '&');
            return std::string_view {value_begin, static_cast<size_t>(value_end - value_begin)};
        }

        key_begin += 1;
    } while (true);

    return std::nullopt;
}

bool is_query(std::string_view uri, std::string_view name) {
    if (uri.size() > name.size())
        return uri.substr(0, name.size()) == name && uri[name.size()] == '?';
    return uri == name;
}

bool validate_column_collections(ArrowSchema* schema_ptr, ArrowArray* column_ptr) {
    // This is safe even in the form of a pointer comparison, doesn't have to be `std::strcmp`.
    if (schema_ptr->format != ustore_doc_field_type_to_arrow_format(ustore_doc_field<ustore_collection_t>()))
        return false;
    if (column_ptr->null_count != 0)
        return false;
    return true;
}

bool validate_column_keys(ArrowSchema* schema_ptr, ArrowArray* column_ptr) {
    // This is safe even in the form of a pointer comparison, doesn't have to be `std::strcmp`.
    if (schema_ptr->format != ustore_doc_field_type_to_arrow_format(ustore_doc_field<ustore_key_t>()))
        return false;
    if (column_ptr->null_count != 0)
        return false;
    return true;
}

bool validate_column_vals(ArrowSchema* schema_ptr, ArrowArray* column_ptr) {
    // This is safe even in the form of a pointer comparison, doesn't have to be `std::strcmp`.
    if (schema_ptr->format != ustore_doc_field_type_to_arrow_format(ustore_doc_field<value_view_t>()))
        return false;
    if (column_ptr->null_count != 0)
        return false;
    return true;
}

class SingleResultStream : public arf::ResultStream {
    std::unique_ptr<arf::Result> one_;

  public:
    SingleResultStream(std::unique_ptr<arf::Result>&& ptr) : one_(std::move(ptr)) {}
    ~SingleResultStream() {}

    ar::Result<std::unique_ptr<arf::Result>> Next() override {
        if (one_)
            return std::exchange(one_, {});
        else
            return {nullptr};
    }
};

class EmptyResultStream : public arf::ResultStream {

  public:
    EmptyResultStream() {}
    ~EmptyResultStream() {}

    ar::Result<std::unique_ptr<arf::Result>> Next() override { return {nullptr}; }
};

/**
 * @brief Wraps a single scalar into a Arrow-compatible `ResultStream`.
 * ## Critique
 * This function marks the pinnacle of ugliness of most modern C++ interfaces.
 * Wrapping an `int` causes 2x `unique_ptr` and a `shared_ptr` allocation!
 */
template <typename scalar_at>
std::unique_ptr<arf::ResultStream> return_scalar(scalar_at const& scalar) {
    static_assert(!std::is_reference_v<scalar_at>);
    auto result = std::make_unique<arf::Result>();
    thread_local scalar_at scalar_copy;
    scalar_copy = scalar;
    result->body = std::make_shared<ar::Buffer>( //
        reinterpret_cast<uint8_t const*>(&scalar_copy),
        sizeof(scalar_copy));
    auto results = std::make_unique<SingleResultStream>(std::move(result));

    return std::unique_ptr<arf::ResultStream>(results.release());
}

std::unique_ptr<arf::ResultStream> return_empty() {
    auto results = std::make_unique<EmptyResultStream>();
    return std::unique_ptr<arf::ResultStream>(results.release());
}

using base_id_t = std::uint64_t;
enum client_id_t : base_id_t {};
enum txn_id_t : base_id_t {};
static_assert(sizeof(txn_id_t) == sizeof(ustore_transaction_t));

client_id_t parse_client_id(arf::ServerCallContext const& ctx) noexcept {
    std::string const& peer_addr = ctx.peer();
    return static_cast<client_id_t>(std::hash<std::string> {}(peer_addr));
}

base_id_t parse_u64_hex(std::string_view str, base_id_t default_ = 0) noexcept {
    // if (str.size() != 16 + 2)
    //     return default_;
    // auto result = boost::lexical_cast<base_id_t>(str.data(), str.size());
    // return result;
    char* end = nullptr;
    base_id_t result = std::strtoull(str.data(), &end, 16);
    if (end != str.end())
        return default_;
    return result;
}

txn_id_t parse_txn_id(std::string_view str) {
    return txn_id_t {parse_u64_hex(str)};
}

base_id_t parse_snap_id(std::string_view str, base_id_t default_ = 0) {
    base_id_t result = default_;
    std::from_chars(str.data(), str.data() + str.size(), result);
    return result;
}

struct session_id_t {
    client_id_t client_id {0};
    txn_id_t txn_id {0};

    bool is_txn() const noexcept { return txn_id; }
    bool operator==(session_id_t const& other) const noexcept {
        return (client_id == other.client_id) & (txn_id == other.txn_id);
    }
    bool operator!=(session_id_t const& other) const noexcept {
        return (client_id != other.client_id) | (txn_id != other.txn_id);
    }
};

struct session_id_hash_t {
    std::size_t operator()(session_id_t const& id) const noexcept {
        std::size_t result = SIZE_MAX;
        hash_combine(result, static_cast<base_id_t>(id.client_id));
        hash_combine(result, static_cast<base_id_t>(id.txn_id));
        return result;
    }
};

/**
 * ## Critique
 * Using `shared_ptr`s inside is not the best design decision,
 * but it boils down to having a good LRU-cache implementation
 * with copy-less lookup possibilities. Neither Boost, nor other
 * popular FOSS C++ implementations have that.
 */
struct running_txn_t {
    ustore_transaction_t txn {};
    ustore_arena_t arena {};
    sys_time_t last_access {};
    bool executing {};
};

using client_to_txn_t = std::unordered_map<session_id_t, running_txn_t, session_id_hash_t>;

struct aging_txn_order_t {
    client_to_txn_t const& sessions;

    bool operator()(session_id_t const& a, session_id_t const& b) const noexcept {
        return sessions.at(a).last_access > sessions.at(b).last_access;
    }
};

class sessions_t;
struct session_lock_t {
    sessions_t& sessions;
    session_id_t session_id;
    ustore_transaction_t txn = nullptr;
    ustore_arena_t arena = nullptr;

    bool is_txn() const noexcept { return txn; }
    ~session_lock_t() noexcept;
};

/**
 * @brief Resource-Allocation control mechanism, that makes sure that no single client
 * holds ownership of any "transaction handle" or "memory arena" for too long. So if
 * a client goes mute or disconnects, we can reuse same memory for other connections
 * and clients.
 */
class sessions_t {
    std::mutex mutex_;
    // Reusable object handles:
    std::vector<ustore_arena_t> free_arenas_;
    std::vector<ustore_transaction_t> free_txns_;
    /// Links each session to memory used for its operations:
    client_to_txn_t client_to_txn_;
    ustore_database_t db_ = nullptr;
    // On Postgre 9.6+ is set to same 30 seconds.
    std::size_t milliseconds_timeout = 30'000;

    running_txn_t pop(ustore_error_t* c_error) noexcept {

        auto it = std::min_element(client_to_txn_.begin(), client_to_txn_.end(), [](auto left, auto right) {
            return left.second.last_access < right.second.last_access && !left.second.executing;
        });

        auto age = std::chrono::duration_cast<std::chrono::milliseconds>(it->second.last_access - sys_clock_t::now());
        if (age.count() < milliseconds_timeout || it->second.executing) {
            log_error_m(c_error, error_unknown_k, "Too many concurrent sessions");
            return {};
        }

        running_txn_t released = it->second;
        client_to_txn_.erase(it);
        released.executing = false;
        return released;
    }

    void submit(session_id_t session_id, running_txn_t running_txn) noexcept {
        running_txn.executing = false;
        auto res = client_to_txn_.insert_or_assign(session_id, running_txn);
    }

  public:
    sessions_t(ustore_database_t db, std::size_t n) : db_(db), free_arenas_(n), free_txns_(n), client_to_txn_(n) {
        std::fill_n(free_arenas_.begin(), n, nullptr);
        std::fill_n(free_txns_.begin(), n, nullptr);
    }

    ~sessions_t() noexcept {
        for (auto a : free_arenas_)
            ustore_arena_free(a);
        for (auto t : free_txns_)
            ustore_transaction_free(t);
    }

    running_txn_t continue_txn(session_id_t session_id, ustore_error_t* c_error) noexcept {
        std::unique_lock _ {mutex_};

        auto it = client_to_txn_.find(session_id);
        if (it == client_to_txn_.end()) {
            log_error_m(c_error, args_wrong_k, "Transaction was terminated, start a new one");
            return {};
        }

        running_txn_t& running = it->second;
        if (running.executing) {
            log_error_m(c_error, args_wrong_k, "Transaction can't be modified concurrently.");
            return {};
        }

        running.executing = true;
        running.last_access = sys_clock_t::now();

        // Update the heap order.
        // With a single change shouldn't take more than `log2(n)` operations.
        return running;
    }

    running_txn_t request_txn(session_id_t session_id, ustore_error_t* c_error) noexcept {
        std::unique_lock _ {mutex_};

        auto it = client_to_txn_.find(session_id);
        if (it != client_to_txn_.end()) {
            log_error_m(c_error, args_wrong_k, "Such transaction is already running, just continue using it.");
            return {};
        }

        // Consider evicting some of the old sessions, if there are no more empty slots
        if (free_txns_.empty() || free_arenas_.empty()) {
            running_txn_t running = pop(c_error);
            if (*c_error)
                return {};
            running.executing = true;
            running.last_access = sys_clock_t::now();
            return running;
        }

        // If we have free slots
        running_txn_t running {};
        running.arena = free_arenas_.back();
        running.txn = free_txns_.back();
        running.executing = true;
        running.last_access = sys_clock_t::now();
        free_arenas_.pop_back();
        free_txns_.pop_back();
        return running;
    }

    void hold_txn(session_id_t session_id, running_txn_t running_txn) noexcept {
        std::unique_lock _ {mutex_};
        submit(session_id, running_txn);
    }

    void release_txn(running_txn_t running_txn) noexcept {
        std::unique_lock _ {mutex_};
        free_arenas_.push_back(running_txn.arena);
        free_txns_.push_back(running_txn.txn);
    }

    void release_txn(session_id_t session_id) noexcept {
        std::unique_lock _ {mutex_};
        auto it = client_to_txn_.find(session_id);
        if (it == client_to_txn_.end())
            return;
        it->second.executing = false;
        free_arenas_.push_back(it->second.arena);
        free_txns_.push_back(it->second.txn);
        client_to_txn_.erase(it);
    }

    ustore_arena_t request_arena(ustore_error_t* c_error) noexcept {
        std::unique_lock _ {mutex_};
        // Consider evicting some of the old sessions, if there are no more empty slots
        if (free_arenas_.empty()) {
            running_txn_t running = pop(c_error);
            if (*c_error)
                return nullptr;
            free_txns_.push_back(running.txn);
            return running.arena;
        }

        ustore_arena_t arena = free_arenas_.back();
        free_arenas_.pop_back();
        return arena;
    }

    void release_arena(ustore_arena_t arena) noexcept {
        std::unique_lock _ {mutex_};
        free_arenas_.push_back(arena);
    }

    session_lock_t lock(session_id_t id, ustore_error_t* c_error) noexcept {
        if (id.is_txn()) {
            running_txn_t running = continue_txn(id, c_error);
            return {*this, id, running.txn, running.arena};
        }
        else
            return {*this, id, nullptr, request_arena(c_error)};
    }
};

session_lock_t::~session_lock_t() noexcept {
    if (is_txn())
        sessions.hold_txn( //
            session_id,
            running_txn_t {txn, arena, sys_clock_t::now(), true});
    else
        sessions.release_arena(arena);
}

struct session_params_t {
    session_id_t session_id;
    std::optional<std::string_view> transaction_id;
    std::optional<std::string_view> snapshot_id;
    std::optional<std::string_view> collection_name;
    std::optional<std::string_view> collection_id;
    std::optional<std::string_view> collection_drop_mode;
    std::optional<std::string_view> read_part;

    std::optional<std::string_view> opt_snapshot;
    std::optional<std::string_view> opt_flush;
    std::optional<std::string_view> opt_dont_watch;
    std::optional<std::string_view> opt_shared_memory;
    std::optional<std::string_view> opt_dont_discard_memory;
};

session_params_t session_params(arf::ServerCallContext const& server_call, std::string_view uri) noexcept {

    session_params_t result;
    result.session_id.client_id = parse_client_id(server_call);

    auto params_offs = uri.find('?');
    if (params_offs == std::string_view::npos)
        return result;

    auto params = uri.substr(params_offs);
    result.transaction_id = param_value(params, kParamTransactionID);
    if (result.transaction_id)
        result.session_id.txn_id = parse_txn_id(*result.transaction_id);

    result.snapshot_id = param_value(params, kParamSnapshotID);

    result.collection_name = param_value(params, kParamCollectionName);
    result.collection_id = param_value(params, kParamCollectionID);

    result.collection_drop_mode = param_value(params, kParamDropMode);
    result.read_part = param_value(params, kParamReadPart);

    result.opt_flush = param_value(params, kParamFlagFlushWrite);
    result.opt_dont_watch = param_value(params, kParamFlagDontWatch);
    result.opt_shared_memory = param_value(params, kParamFlagSharedMemRead);

    // This flag shouldn't have been forwarded to the server.
    // In standalone builds it remains on the client.
    // result.opt_dont_discard_memory = param_value(params, kParamFlagDontDiscard);
    return result;
}

ustore_options_t ustore_options(session_params_t const& params) noexcept {
    ustore_options_t result = ustore_options_default_k;
    if (params.opt_dont_watch)
        result = ustore_options_t(result | ustore_option_transaction_dont_watch_k);
    if (params.opt_flush)
        result = ustore_options_t(result | ustore_option_write_flush_k);
    if (params.opt_shared_memory)
        result = ustore_options_t(result | ustore_option_read_shared_memory_k);
    return result;
}

ustore_str_view_t get_null_terminated(ar::Buffer const& buf) noexcept {
    ustore_str_view_t collection_config = reinterpret_cast<ustore_str_view_t>(buf.data());
    auto end_config = collection_config + buf.capacity();
    return std::find(collection_config, end_config, '\0') == end_config ? nullptr : collection_config;
}

ustore_str_view_t get_null_terminated(std::shared_ptr<ar::Buffer> const& buf_ptr) noexcept {
    return buf_ptr ? get_null_terminated(*buf_ptr) : nullptr;
}

/**
 * @brief Remote Procedure Call implementation on top of Apache Arrow Flight RPC.
 * Currently only implements only the binary interface, which is enough even for
 * Document and Graph logic to work properly with most of encoding/decoding
 * shifted to client side.
 *
 * ## Endpoints
 *
 * - write?col=x&txn=y&lengths&watch&shared (DoPut)
 * - read?col=x&txn=y&flush (DoExchange)
 * - collection_upsert?col=x (DoAction): Returns collection ID
 *   Payload buffer: Collection opening config.
 * - collection_remove?col=x (DoAction): Drops a collection
 * - txn_begin?txn=y (DoAction): Starts a transaction with a potentially custom ID
 * - txn_commit?txn=y (DoAction): Commits a transaction with a given ID
 *
 * ## Concurrency
 *
 * Flight RPC allows concurrent calls from the same client.
 * In our implementation things are trickier, as transactions are not thread-safe.
 */
class UStoreService : public arf::FlightServerBase {
    database_t db_;
    sessions_t sessions_;

  public:
    UStoreService(database_t&& db, std::size_t capacity = 4096) : db_(std::move(db)), sessions_(db_, capacity) {}

    ar::Status ListActions( //
        arf::ServerCallContext const&,
        std::vector<arf::ActionType>* actions) override {
        *actions =
            {kActionColOpen, kActionColDrop, kActionSnapOpen, kActionSnapDrop, kActionTxnBegin, kActionTxnCommit};
        return ar::Status::OK();
    }

    ar::Status ListFlights( //
        arf::ServerCallContext const&,
        arf::Criteria const*,
        std::unique_ptr<arf::FlightListing>*) override {
        return ar::Status::OK();
    }

    ar::Status GetFlightInfo( //
        arf::ServerCallContext const&,
        arf::FlightDescriptor const&,
        std::unique_ptr<arf::FlightInfo>*) override {
        // ARROW_ASSIGN_OR_RAISE(auto file_info, FileInfoFromDescriptor(descriptor));
        // ARROW_ASSIGN_OR_RAISE(auto flight_info, MakeFlightInfo(file_info));
        // *info = std::make_unique<arf::FlightInfo>(std::move(flight_info));
        return ar::Status::OK();
    }

    ar::Status GetSchema( //
        arf::ServerCallContext const&,
        arf::FlightDescriptor const&,
        std::unique_ptr<arf::SchemaResult>*) override {
        return ar::Status::OK();
    }

    ar::Status DoAction( //
        arf::ServerCallContext const& server_call,
        arf::Action const& action,
        std::unique_ptr<arf::ResultStream>* results_ptr) override {

        ar::Status ar_status;
        session_params_t params = session_params(server_call, action.type);
        status_t status;

        // Locating the collection ID
        if (is_query(action.type, kActionColOpen.type)) {
            if (!params.collection_name)
                return ar::Status::Invalid("Missing collection name argument");

            // The name must be null-terminated.
            // This is not safe:
            ustore_str_span_t c_collection_name = nullptr;
            c_collection_name = (ustore_str_span_t)params.collection_name->begin();
            c_collection_name[params.collection_name->size()] = 0;

            ustore_collection_t collection_id = 0;
            ustore_str_view_t collection_config = get_null_terminated(action.body);
            ustore_collection_create_t collection_init {};
            collection_init.db = db_;
            collection_init.error = status.member_ptr();
            collection_init.name = params.collection_name->begin();
            collection_init.config = collection_config;
            collection_init.id = &collection_id;

            ustore_collection_create(&collection_init);
            if (!status)
                return ar::Status::ExecutionError(status.message());

            *results_ptr = return_scalar<ustore_collection_t>(collection_id);
            return ar::Status::OK();
        }

        // Dropping a collection
        if (is_query(action.type, kActionColDrop.type)) {
            if (!params.collection_id)
                return ar::Status::Invalid("Missing collection ID argument");

            ustore_drop_mode_t mode =                               //
                params.collection_drop_mode == kParamDropModeValues //
                    ? ustore_drop_vals_k
                    : params.collection_drop_mode == kParamDropModeContents //
                          ? ustore_drop_keys_vals_k
                          : ustore_drop_keys_vals_handle_k;

            ustore_collection_t c_collection_id = ustore_collection_main_k;
            if (params.collection_id)
                c_collection_id = parse_u64_hex(*params.collection_id, ustore_collection_main_k);

            ustore_collection_drop_t collection_drop {};
            collection_drop.db = db_;
            collection_drop.error = status.member_ptr();
            collection_drop.id = c_collection_id;
            collection_drop.mode = mode;

            ustore_collection_drop(&collection_drop);
            if (!status)
                return ar::Status::ExecutionError(status.message());
            *results_ptr = return_empty();
            return ar::Status::OK();
        }

        // Create a snapshot
        if (is_query(action.type, kActionSnapOpen.type)) {
            if (params.snapshot_id)
                return ar::Status::Invalid("Missing snapshot ID argument");

            ustore_snapshot_t snapshot_id = 0;
            ustore_snapshot_create_t snapshot_create {
                .db = db_,
                .error = status.member_ptr(),
                .id = &snapshot_id,
            };

            ustore_snapshot_create(&snapshot_create);
            if (!status)
                return ar::Status::ExecutionError(status.message());

            *results_ptr = return_scalar<ustore_snapshot_t>(snapshot_id);
            return ar::Status::OK();
        }

        // Dropping a snapshot
        if (is_query(action.type, kActionSnapDrop.type)) {
            if (!params.snapshot_id)
                return ar::Status::Invalid("Missing snapshot ID argument");

            ustore_snapshot_t c_snapshot_id = 0;
            if (params.snapshot_id)
                c_snapshot_id = parse_snap_id(*params.snapshot_id);

            ustore_snapshot_drop_t snapshot_drop {
                .db = db_,
                .error = status.member_ptr(),
                .id = c_snapshot_id,
            };

            ustore_snapshot_drop(&snapshot_drop);
            if (!status)
                return ar::Status::ExecutionError(status.message());
            *results_ptr = return_empty();
            return ar::Status::OK();
        }

        // Starting a transaction
        if (is_query(action.type, kActionTxnBegin.type)) {
            if (!params.transaction_id)
                params.session_id.txn_id = static_cast<txn_id_t>(std::rand());

            // Request handles for memory
            running_txn_t session = sessions_.request_txn(params.session_id, status.member_ptr());
            if (!status)
                return ar::Status::ExecutionError(status.message());

            // Cleanup internal state
            ustore_transaction_init_t txn_init {};
            txn_init.db = db_;
            txn_init.error = status.member_ptr();
            txn_init.options = ustore_options(params);
            txn_init.transaction = &session.txn;

            ustore_transaction_init(&txn_init);
            if (!status) {
                sessions_.release_txn(params.session_id);
                return ar::Status::ExecutionError(status.message());
            }

            // Don't forget to add the transaction to active sessions
            sessions_.hold_txn(params.session_id, session);
            *results_ptr = return_scalar<txn_id_t>(params.session_id.txn_id);
            return ar::Status::OK();
        }

        if (is_query(action.type, kActionTxnCommit.type)) {
            if (!params.transaction_id)
                return ar::Status::Invalid("Missing transaction ID argument");

            running_txn_t session = sessions_.continue_txn(params.session_id, status.member_ptr());
            if (!status) {
                sessions_.release_txn(params.session_id);
                return ar::Status::ExecutionError(status.message());
            }

            ustore_transaction_commit_t txn_commit {};
            txn_commit.db = db_;
            txn_commit.error = status.member_ptr();
            txn_commit.transaction = session.txn;
            txn_commit.options = ustore_options(params);

            ustore_transaction_commit(&txn_commit);
            if (!status) {
                sessions_.release_txn(params.session_id);
                return ar::Status::ExecutionError(status.message());
            }

            sessions_.release_txn(params.session_id);
            *results_ptr = return_empty();
            return ar::Status::OK();
        }

        return ar::Status::NotImplemented("Unknown action type: ", action.type);
    }

    ar::Status DoExchange( //
        arf::ServerCallContext const& server_call,
        std::unique_ptr<arf::FlightMessageReader> request_ptr,
        std::unique_ptr<arf::FlightMessageWriter> response_ptr) override {

        ar::Status ar_status;
        arf::FlightMessageReader& request = *request_ptr;
        arf::FlightMessageWriter& response = *response_ptr;
        arf::FlightDescriptor const& desc = request.descriptor();
        session_params_t params = session_params(server_call, desc.cmd);
        status_t status;

        ArrowSchema input_schema_c, output_schema_c;
        ArrowArray input_batch_c, output_batch_c;
        if (ar_status = unpack_table(request.ToTable(), input_schema_c, input_batch_c); !ar_status.ok())
            return ar_status;

        bool is_empty_values = false;

        /// @param `collections`
        ustore_collection_t c_collection_id = ustore_collection_main_k;
        strided_iterator_gt<ustore_collection_t> input_collections;
        if (params.collection_id) {
            c_collection_id = parse_u64_hex(*params.collection_id, ustore_collection_main_k);
            input_collections = strided_iterator_gt<ustore_collection_t> {&c_collection_id};
        }
        else
            input_collections = get_collections(input_schema_c, input_batch_c, kArgCols);

        ustore_snapshot_t c_snapshot_id = 0;
        if (params.snapshot_id)
            c_snapshot_id = parse_snap_id(*params.snapshot_id);

        // Reserve resources for the execution of this request
        auto session = sessions_.lock(params.session_id, status.member_ptr());
        if (!status)
            return ar::Status::ExecutionError(status.message());

        if (is_query(desc.cmd, kFlightRead)) {

            /// @param `keys`
            auto input_keys = get_keys(input_schema_c, input_batch_c, kArgKeys);
            if (!input_keys)
                return ar::Status::Invalid("Keys must have been provided for reads");

            bool const request_only_presences = params.read_part == kParamReadPartPresences;
            bool const request_only_lengths = params.read_part == kParamReadPartLengths;
            bool const request_content = !request_only_lengths && !request_only_presences;

            if (!status)
                return ar::Status::ExecutionError(status.message());

            // As we are immediately exporting in the Arrow format,
            // we don't need the lengths, just the NULL indicators
            ustore_bytes_ptr_t found_values = nullptr;
            ustore_length_t* found_offsets = nullptr;
            ustore_length_t* found_lengths = nullptr;
            ustore_octet_t* found_presences = nullptr;
            ustore_size_t tasks_count = static_cast<ustore_size_t>(input_batch_c.length);
            ustore_read_t read {};
            read.db = db_;
            read.error = status.member_ptr();
            read.transaction = session.txn;
            read.snapshot = c_snapshot_id;
            read.arena = &session.arena;
            read.options = ustore_options(params);
            read.tasks_count = tasks_count;
            read.collections = input_collections.get();
            read.collections_stride = input_collections.stride();
            read.keys = input_keys.get();
            read.keys_stride = input_keys.stride();
            read.presences = &found_presences;
            read.offsets = request_content ? &found_offsets : nullptr;
            read.lengths = request_only_lengths ? &found_lengths : nullptr;
            read.values = request_content ? &found_values : nullptr;

            ustore_read(&read);
            if (!status)
                return ar::Status::ExecutionError(status.message());

            is_empty_values = request_content && (found_values == nullptr);

            ustore_size_t result_length =
                request_only_presences ? divide_round_up<ustore_size_t>(tasks_count, CHAR_BIT) : tasks_count;
            ustore_to_arrow_schema(result_length, 1, &output_schema_c, &output_batch_c, status.member_ptr());
            if (!status)
                return ar::Status::ExecutionError(status.message());

            if (request_content)
                ustore_to_arrow_column( //
                    result_length,
                    kArgVals.c_str(),
                    ustore_doc_field_bin_k,
                    found_presences,
                    found_offsets,
                    found_values,
                    output_schema_c.children[0],
                    output_batch_c.children[0],
                    status.member_ptr());
            else if (request_only_lengths)
                ustore_to_arrow_column( //
                    result_length,
                    kArgLengths.c_str(),
                    ustore_doc_field<ustore_length_t>(),
                    found_presences,
                    nullptr,
                    found_lengths,
                    output_schema_c.children[0],
                    output_batch_c.children[0],
                    status.member_ptr());
            else if (request_only_presences)
                ustore_to_arrow_column( //
                    result_length,
                    kArgPresences.c_str(),
                    ustore_doc_field<ustore_octet_t>(),
                    nullptr,
                    nullptr,
                    found_presences,
                    output_schema_c.children[0],
                    output_batch_c.children[0],
                    status.member_ptr());
            if (!status)
                return ar::Status::ExecutionError(status.message());
        }
        else if (is_query(desc.cmd, kFlightReadPath)) {

            /// @param `keys`
            auto input_paths = get_contents(input_schema_c, input_batch_c, kArgPaths.c_str());
            if (!input_paths.contents_begin)
                return ar::Status::Invalid("Keys must have been provided for reads");

            bool const request_only_presences = params.read_part == kParamReadPartPresences;
            bool const request_only_lengths = params.read_part == kParamReadPartLengths;
            bool const request_content = !request_only_lengths && !request_only_presences;

            // As we are immediately exporting in the Arrow format,
            // we don't need the lengths, just the NULL indicators
            ustore_bytes_ptr_t found_values = nullptr;
            ustore_length_t* found_offsets = nullptr;
            ustore_length_t* found_lengths = nullptr;
            ustore_octet_t* found_presences = nullptr;
            ustore_size_t tasks_count = static_cast<ustore_size_t>(input_batch_c.length);
            ustore_paths_read_t read {};
            read.db = db_;
            read.error = status.member_ptr();
            read.transaction = session.txn;
            read.arena = &session.arena;
            read.options = ustore_options(params);
            read.tasks_count = tasks_count;
            read.path_separator = input_paths.separator;
            read.collections = input_collections.get();
            read.collections_stride = input_collections.stride();
            read.paths = reinterpret_cast<ustore_str_view_t const*>(input_paths.contents_begin.get());
            read.paths_stride = input_paths.contents_begin.stride();
            read.paths_offsets = input_paths.offsets_begin.get();
            read.paths_offsets_stride = input_paths.offsets_begin.stride();
            read.paths_lengths = input_paths.lengths_begin.get();
            read.paths_lengths_stride = input_paths.lengths_begin.stride();
            read.presences = &found_presences;
            read.offsets = request_content ? &found_offsets : nullptr;
            read.lengths = request_only_lengths ? &found_lengths : nullptr;
            read.values = request_content ? &found_values : nullptr;

            ustore_paths_read(&read);
            if (!status)
                return ar::Status::ExecutionError(status.message());

            ustore_size_t result_length =
                request_only_presences ? divide_round_up<ustore_size_t>(tasks_count, CHAR_BIT) : tasks_count;
            ustore_to_arrow_schema(result_length, 1, &output_schema_c, &output_batch_c, status.member_ptr());
            if (!status)
                return ar::Status::ExecutionError(status.message());
            if (request_content)
                ustore_to_arrow_column( //
                    result_length,
                    kArgVals.c_str(),
                    ustore_doc_field_bin_k,
                    found_presences,
                    found_offsets,
                    found_values,
                    output_schema_c.children[0],
                    output_batch_c.children[0],
                    status.member_ptr());
            else if (request_only_lengths)
                ustore_to_arrow_column( //
                    result_length,
                    kArgLengths.c_str(),
                    ustore_doc_field<ustore_length_t>(),
                    found_presences,
                    nullptr,
                    found_lengths,
                    output_schema_c.children[0],
                    output_batch_c.children[0],
                    status.member_ptr());
            else if (request_only_presences)
                ustore_to_arrow_column( //
                    result_length,
                    kArgPresences.c_str(),
                    ustore_doc_field<ustore_octet_t>(),
                    nullptr,
                    nullptr,
                    found_presences,
                    output_schema_c.children[0],
                    output_batch_c.children[0],
                    status.member_ptr());
            if (!status)
                return ar::Status::ExecutionError(status.message());
        }
        else if (is_query(desc.cmd, kFlightMatchPath)) {
            /// @param `previous`
            auto input_prevs = get_contents(input_schema_c, input_batch_c, kArgPrevPatterns.c_str());

            /// @param `patterns`
            auto input_patrns = get_contents(input_schema_c, input_batch_c, kArgPatterns.c_str());
            if (!input_patrns.contents_begin)
                return ar::Status::Invalid("Patterns must have been provided for reads");

            /// @param `limits`
            auto input_limits = get_lengths(input_schema_c, input_batch_c, kArgCountLimits);

            bool const request_only_counts = params.read_part == kParamReadPartLengths;
            bool const request_content = !request_only_counts;

            // As we are immediately exporting in the Arrow format,
            // we don't need the lengths, just the NULL indicators
            ustore_char_t* found_values = nullptr;
            ustore_length_t* found_offsets = nullptr;
            ustore_length_t* found_counts = nullptr;
            ustore_size_t tasks_count = static_cast<ustore_size_t>(input_batch_c.length);
            ustore_paths_match_t match {};
            match.db = db_;
            match.error = status.member_ptr();
            match.transaction = session.txn;
            match.arena = &session.arena;
            match.options = ustore_options(params);
            match.tasks_count = tasks_count;
            match.path_separator = input_patrns.separator;
            match.collections = input_collections.get();
            match.collections_stride = input_collections.stride();
            match.patterns = reinterpret_cast<ustore_str_view_t const*>(input_patrns.contents_begin.get());
            match.patterns_stride = input_patrns.contents_begin.stride();
            match.patterns_offsets = input_patrns.offsets_begin.get();
            match.patterns_offsets_stride = input_patrns.offsets_begin.stride();
            match.patterns_lengths = input_patrns.lengths_begin.get();
            match.patterns_lengths_stride = input_patrns.lengths_begin.stride();
            match.match_counts_limits = input_limits.get();
            match.match_counts_limits_stride = input_limits.stride();
            match.previous = reinterpret_cast<ustore_str_view_t const*>(input_prevs.contents_begin.get());
            match.previous_stride = input_prevs.contents_begin.stride();
            match.previous_offsets = input_prevs.offsets_begin.get();
            match.previous_offsets_stride = input_prevs.offsets_begin.stride();
            match.previous_lengths = input_prevs.lengths_begin.get();
            match.previous_lengths_stride = input_prevs.lengths_begin.stride();
            match.match_counts = &found_counts;
            match.paths_offsets = request_content ? &found_offsets : nullptr;
            match.paths_strings = request_content ? &found_values : nullptr;

            ustore_paths_match(&match);
            if (!status)
                return ar::Status::ExecutionError(status.message());

            auto arena = linked_memory(&session.arena, ustore_options_default_k, status.member_ptr());
            if (!status)
                return ar::Status::ExecutionError(status.message());

            ustore_size_t result_length = std::accumulate(found_counts, found_counts + tasks_count, 0);
            auto rounded_counts = arena.alloc<ustore_length_t>(result_length, status.member_ptr());
            if (!status)
                return ar::Status::ExecutionError(status.message());
            void const* values_ptr = result_length ? reinterpret_cast<void const*>(found_values)
                                                   : reinterpret_cast<void const*>(&zero_size_data_k);

            if (rounded_counts)
                std::copy(found_counts, found_counts + tasks_count, rounded_counts.begin());
            else {
                rounded_counts = arena.alloc<ustore_length_t>(1, status.member_ptr());
                if (!status)
                    return ar::Status::ExecutionError(status.message());
                result_length = 1;
            }

            ustore_size_t collections_count = 1 + request_content;
            ustore_to_arrow_schema(result_length,
                                   collections_count,
                                   &output_schema_c,
                                   &output_batch_c,
                                   status.member_ptr());
            if (!status)
                return ar::Status::ExecutionError(status.message());
            ustore_to_arrow_column( //
                result_length,
                kArgLengths.c_str(),
                ustore_doc_field<ustore_length_t>(),
                nullptr,
                nullptr,
                rounded_counts.begin(),
                output_schema_c.children[0],
                output_batch_c.children[0],
                status.member_ptr());
            if (request_content)
                ustore_to_arrow_column( //
                    result_length,
                    kArgVals.c_str(),
                    ustore_doc_field_bin_k,
                    nullptr,
                    found_offsets,
                    values_ptr,
                    output_schema_c.children[1],
                    output_batch_c.children[1],
                    status.member_ptr());

            if (!status)
                return ar::Status::ExecutionError(status.message());
        }
        else if (is_query(desc.cmd, kFlightScan)) {

            /// @param `start_keys`
            auto input_start_keys = get_keys(input_schema_c, input_batch_c, kArgScanStarts);
            /// @param `lengths`
            auto input_lengths = get_lengths(input_schema_c, input_batch_c, kArgCountLimits);

            if (!input_start_keys || !input_lengths)
                return ar::Status::Invalid("Keys and lengths must have been provided for scans");

            // As we are immediately exporting in the Arrow format,
            // we don't need the lengths, just the NULL indicators
            ustore_length_t* found_offsets = nullptr;
            ustore_length_t* found_lengths = nullptr;
            ustore_length_t* found_counts = nullptr;
            ustore_key_t* found_keys = nullptr;
            ustore_size_t tasks_count = static_cast<ustore_size_t>(input_batch_c.length);
            ustore_scan_t scan {};
            scan.db = db_;
            scan.error = status.member_ptr();
            scan.transaction = session.txn;
            scan.arena = &session.arena;
            scan.options = ustore_options(params);
            scan.tasks_count = tasks_count;
            scan.collections = input_collections.get();
            scan.collections_stride = input_collections.stride();
            scan.start_keys = input_start_keys.get();
            scan.start_keys_stride = input_start_keys.stride();
            scan.count_limits = input_lengths.get();
            scan.count_limits_stride = input_lengths.stride();
            scan.offsets = &found_offsets;
            scan.keys = &found_keys;
            scan.counts = &found_counts;

            ustore_scan(&scan);
            if (!status)
                return ar::Status::ExecutionError(status.message());

            ustore_to_arrow_schema(found_offsets[tasks_count],
                                   2,
                                   &output_schema_c,
                                   &output_batch_c,
                                   status.member_ptr());
            if (!status)
                return ar::Status::ExecutionError(status.message());

            ustore_to_arrow_column( //
                found_offsets[tasks_count],
                kArgKeys.c_str(),
                ustore_doc_field<ustore_key_t>(),
                nullptr,
                nullptr,
                found_keys,
                output_schema_c.children[0],
                output_batch_c.children[0],
                status.member_ptr());
            if (!status)
                return ar::Status::ExecutionError(status.message());

            ustore_to_arrow_column( //
                found_offsets[tasks_count],
                "offsets",
                ustore_doc_field<ustore_key_t>(),
                nullptr,
                nullptr,
                found_offsets,
                output_schema_c.children[1],
                output_batch_c.children[1],
                status.member_ptr());
            if (!status)
                return ar::Status::ExecutionError(status.message());
        }
        else if (is_query(desc.cmd, kFlightSample)) {

            /// @param `limits`
            auto input_limits = get_lengths(input_schema_c, input_batch_c, kArgCountLimits);

            if (!input_limits)
                return ar::Status::Invalid("Limits must have been provided for sampling");

            // As we are immediately exporting in the Arrow format,
            // we don't need the lengths, just the NULL indicators
            ustore_length_t* found_offsets = nullptr;
            ustore_length_t* found_lengths = nullptr;
            ustore_length_t* found_counts = nullptr;
            ustore_key_t* found_keys = nullptr;
            ustore_size_t tasks_count = static_cast<ustore_size_t>(input_batch_c.length);
            ustore_sample_t sample {};
            sample.db = db_;
            sample.error = status.member_ptr();
            sample.transaction = session.txn;
            sample.arena = &session.arena;
            sample.options = ustore_options(params);
            sample.tasks_count = tasks_count;
            sample.collections = input_collections.get();
            sample.collections_stride = input_collections.stride();
            sample.count_limits = input_limits.get();
            sample.count_limits_stride = input_limits.stride();
            sample.offsets = &found_offsets;
            sample.keys = &found_keys;
            sample.counts = &found_counts;

            ustore_sample(&sample);
            if (!status)
                return ar::Status::ExecutionError(status.message());

            ustore_to_arrow_schema(found_offsets[tasks_count],
                                   2,
                                   &output_schema_c,
                                   &output_batch_c,
                                   status.member_ptr());
            if (!status)
                return ar::Status::ExecutionError(status.message());

            ustore_to_arrow_column( //
                found_offsets[tasks_count],
                kArgKeys.c_str(),
                ustore_doc_field<ustore_key_t>(),
                nullptr,
                nullptr,
                found_keys,
                output_schema_c.children[0],
                output_batch_c.children[0],
                status.member_ptr());
            if (!status)
                return ar::Status::ExecutionError(status.message());

            ustore_to_arrow_column( //
                found_offsets[tasks_count],
                "offsets",
                ustore_doc_field<ustore_key_t>(),
                nullptr,
                nullptr,
                found_offsets,
                output_schema_c.children[1],
                output_batch_c.children[1],
                status.member_ptr());
        }

        if (is_empty_values)
            output_batch_c.children[0]->buffers[2] = &zero_size_data_k;
        arrow::Result<std::shared_ptr<arrow::RecordBatch>> maybe_table =
            ar::ImportRecordBatch(&output_batch_c, &output_schema_c);

        if (!maybe_table.ok())
            return maybe_table.status();

        auto table = maybe_table.ValueUnsafe();
        ar_status = table->ValidateFull();
        if (!ar_status.ok())
            return ar_status;

        ar_status = response.Begin(table->schema());
        if (!ar_status.ok())
            return ar_status;

        ar_status = response.WriteRecordBatch(*table);
        if (!ar_status.ok())
            return ar_status;

        return response.Close();
    }

    ar::Status DoPut( //
        arf::ServerCallContext const& server_call,
        std::unique_ptr<arf::FlightMessageReader> request_ptr,
        std::unique_ptr<arf::FlightMetadataWriter> response_ptr) override {

        ar::Status ar_status;
        arf::FlightMessageReader& request = *request_ptr;
        arf::FlightMetadataWriter& response = *response_ptr;
        arf::FlightDescriptor const& desc = request.descriptor();
        session_params_t params = session_params(server_call, desc.cmd);
        status_t status;

        ArrowSchema input_schema_c;
        ArrowArray input_batch_c;
        if (ar_status = unpack_table(request.ToTable(), input_schema_c, input_batch_c); !ar_status.ok())
            return ar_status;

        if (is_query(desc.cmd, kFlightWrite)) {

            /// @param `keys`
            auto input_keys = get_keys(input_schema_c, input_batch_c, kArgKeys);
            if (!input_keys)
                return ar::Status::Invalid("Keys must have been provided for reads");

            /// @param `collections`
            ustore_collection_t c_collection_id = ustore_collection_main_k;
            strided_iterator_gt<ustore_collection_t> input_collections;
            if (params.collection_id) {
                c_collection_id = parse_u64_hex(*params.collection_id, ustore_collection_main_k);
                input_collections = strided_iterator_gt<ustore_collection_t> {&c_collection_id};
            }
            else
                input_collections = get_collections(input_schema_c, input_batch_c, kArgCols);

            auto input_vals = get_contents(input_schema_c, input_batch_c, kArgVals);

            auto session = sessions_.lock(params.session_id, status.member_ptr());
            if (!status)
                return ar::Status::ExecutionError(status.message());

            ustore_size_t tasks_count = static_cast<ustore_size_t>(input_batch_c.length);
            ustore_write_t write {};
            write.db = db_;
            write.error = status.member_ptr();
            write.transaction = session.txn;
            write.arena = &session.arena;
            write.options = ustore_options(params);
            write.tasks_count = tasks_count;
            write.collections = input_collections.get();
            write.collections_stride = input_collections.stride();
            write.keys = input_keys.get();
            write.keys_stride = input_keys.stride();
            write.presences = input_vals.presences_begin.get();
            write.offsets = input_vals.offsets_begin.get();
            write.offsets_stride = input_vals.offsets_begin.stride();
            write.lengths = input_vals.lengths_begin.get();
            write.lengths_stride = input_vals.lengths_begin.stride();
            write.values = input_vals.contents_begin.get();
            write.values_stride = input_vals.contents_begin.stride();

            ustore_write(&write);

            if (!status)
                return ar::Status::ExecutionError(status.message());
        }
        else if (is_query(desc.cmd, kFlightWritePath)) {
            /// @param `keys`
            auto input_paths = get_contents(input_schema_c, input_batch_c, kArgPaths.c_str());
            if (!input_paths.contents_begin)
                return ar::Status::Invalid("Keys must have been provided for reads");

            /// @param `collections`
            ustore_collection_t c_collection_id = ustore_collection_main_k;
            strided_iterator_gt<ustore_collection_t> input_collections;
            if (params.collection_id) {
                c_collection_id = parse_u64_hex(*params.collection_id, ustore_collection_main_k);
                input_collections = strided_iterator_gt<ustore_collection_t> {&c_collection_id};
            }
            else
                input_collections = get_collections(input_schema_c, input_batch_c, kArgCols);

            auto input_vals = get_contents(input_schema_c, input_batch_c, kArgVals);

            auto session = sessions_.lock(params.session_id, status.member_ptr());
            if (!status)
                return ar::Status::ExecutionError(status.message());

            ustore_size_t tasks_count = static_cast<ustore_size_t>(input_batch_c.length);
            ustore_paths_write_t write {};
            write.db = db_;
            write.error = status.member_ptr();
            write.transaction = session.txn;
            write.arena = &session.arena;
            write.options = ustore_options(params);
            write.tasks_count = tasks_count;
            write.path_separator = input_paths.separator;
            write.collections = input_collections.get();
            write.collections_stride = input_collections.stride();
            write.paths = reinterpret_cast<ustore_str_view_t const*>(input_paths.contents_begin.get());
            write.paths_stride = input_paths.contents_begin.stride();
            write.paths_offsets = input_paths.offsets_begin.get();
            write.paths_offsets_stride = input_paths.offsets_begin.stride();
            write.paths_lengths = input_paths.lengths_begin.get();
            write.paths_lengths_stride = input_paths.lengths_begin.stride();
            write.values_presences = input_vals.presences_begin.get();
            write.values_offsets = input_vals.offsets_begin.get();
            write.values_offsets_stride = input_vals.offsets_begin.stride();
            write.values_lengths = input_vals.lengths_begin.get();
            write.values_lengths_stride = input_vals.lengths_begin.stride();
            write.values_bytes = input_vals.contents_begin.get();
            write.values_bytes_stride = input_vals.contents_begin.stride();

            ustore_paths_write(&write);

            if (!status)
                return ar::Status::ExecutionError(status.message());
        }
        return ar::Status::OK();
    }

    ar::Status DoGet( //
        arf::ServerCallContext const& server_call,
        arf::Ticket const& ticket,
        std::unique_ptr<arf::FlightDataStream>* response_ptr) override {

        ar::Status ar_status;
        session_params_t params = session_params(server_call, ticket.ticket);
        status_t status;

        if (is_query(ticket.ticket, kFlightListCols)) {

            // We will need some temporary memory for exports
            auto session = sessions_.lock(params.session_id, status.member_ptr());
            if (!status)
                return ar::Status::ExecutionError(status.message());

            ustore_size_t count = 0;
            ustore_collection_t* collections = nullptr;
            ustore_length_t* offsets = nullptr;
            ustore_str_span_t names = nullptr;
            ustore_collection_list_t collection_list {};
            collection_list.db = db_;
            collection_list.error = status.member_ptr();
            collection_list.transaction = session.txn;
            collection_list.snapshot = {}; // TODO
            collection_list.arena = &session.arena;
            collection_list.options = ustore_options(params);
            collection_list.count = &count;
            collection_list.ids = &collections;
            collection_list.offsets = &offsets;
            collection_list.names = &names;

            ustore_collection_list(&collection_list);
            if (!status)
                return ar::Status::ExecutionError(status.message());

            // Pack two columns into a Table
            ArrowSchema schema_c;
            ArrowArray array_c;
            ustore_to_arrow_schema(count, 2, &schema_c, &array_c, status.member_ptr());
            if (!status)
                return ar::Status::ExecutionError(status.message());

            ustore_to_arrow_column( //
                count,
                kArgCols.c_str(),
                ustore_doc_field<ustore_collection_t>(),
                nullptr,
                nullptr,
                ustore_bytes_ptr_t(collections),
                schema_c.children[0],
                array_c.children[0],
                status.member_ptr());
            if (!status)
                return ar::Status::ExecutionError(status.message());

            ustore_to_arrow_column( //
                count,
                kArgNames.c_str(),
                ustore_doc_field_str_k,
                nullptr,
                offsets,
                ustore_bytes_ptr_t(names),
                schema_c.children[1],
                array_c.children[1],
                status.member_ptr());
            if (!status)
                return ar::Status::ExecutionError(status.message());

            auto maybe_batch = ar::ImportRecordBatch(&array_c, &schema_c);
            if (!maybe_batch.ok())
                return maybe_batch.status();

            auto batch = maybe_batch.ValueUnsafe();
            auto maybe_reader = ar::RecordBatchReader::Make({batch});
            if (!maybe_reader.ok())
                return maybe_reader.status();

            // TODO: Pass right IPC options
            auto stream = std::make_unique<arf::RecordBatchStream>(maybe_reader.ValueUnsafe());
            *response_ptr = std::move(stream);
            return ar::Status::OK();
        }
        else if (is_query(ticket.ticket, kFlightListSnap)) {
            // We will need some temporary memory for exports
            auto session = sessions_.lock(params.session_id, status.member_ptr());
            if (!status)
                return ar::Status::ExecutionError(status.message());

            ustore_size_t count = 0;
            ustore_snapshot_t* snapshots = nullptr;
            ustore_snapshot_list_t snapshots_list;
            snapshots_list.db = db_;
            snapshots_list.error = status.member_ptr();
            snapshots_list.arena = &session.arena;
            snapshots_list.options = ustore_options(params);
            snapshots_list.count = &count;
            snapshots_list.ids = &snapshots;

            ustore_snapshot_list(&snapshots_list);
            if (!status)
                return ar::Status::ExecutionError(status.message());

            if (count == 0)
                return ar::Status::OK();

            // Pack two columns into a Table
            ArrowSchema schema_c;
            ArrowArray array_c;
            ustore_to_arrow_schema(count, 2, &schema_c, &array_c, status.member_ptr());
            if (!status)
                return ar::Status::ExecutionError(status.message());

            ustore_to_arrow_column( //
                count,
                kArgSnaps.c_str(),
                ustore_doc_field<ustore_snapshot_t>(),
                nullptr,
                nullptr,
                ustore_bytes_ptr_t(snapshots),
                schema_c.children[0],
                array_c.children[0],
                status.member_ptr());
            if (!status)
                return ar::Status::ExecutionError(status.message());

            auto maybe_batch = ar::ImportRecordBatch(&array_c, &schema_c);
            if (!maybe_batch.ok())
                return maybe_batch.status();

            auto batch = maybe_batch.ValueUnsafe();
            auto maybe_reader = ar::RecordBatchReader::Make({batch});
            if (!maybe_reader.ok())
                return maybe_reader.status();

            // TODO: Pass right IPC options
            auto stream = std::make_unique<arf::RecordBatchStream>(maybe_reader.ValueUnsafe());
            *response_ptr = std::move(stream);
            return ar::Status::OK();
        }
        return ar::Status::OK();
    }
};

ar::Status run_server(ustore_str_view_t config, int port, bool quiet) {

    database_t db;
    db.open(config).throw_unhandled();

    arf::Location server_location = arf::Location::ForGrpcTcp("0.0.0.0", port).ValueUnsafe();
    arf::FlightServerOptions options(server_location);

    status_t status;
    ustore_arena_t c_arena(db);
    linked_memory_lock_t arena = linked_memory(&c_arena, ustore_options_default_k, status.member_ptr());
    if (!status)
        return ar::Status::ExecutionError(status.message());
    arrow_mem_pool_t pool(arena);
    options.memory_manager = ar::CPUDevice::memory_manager(&pool);

    auto server = std::make_unique<UStoreService>(std::move(db));
    ARROW_RETURN_NOT_OK(server->Init(options));
    if (!quiet)
        std::printf("Listening on port: %i\n", server->port());
    return server->Serve();
}

//------------------------------------------------------------------------------

int main(int argc, char* argv[]) {

    using namespace clipp;

    std::string config_path = "/var/lib/ustore/config.json";
    int port = 38709;
    bool quiet = false;
    bool help = false;

    auto cli = ( //
        (option("--config") & value("path", config_path))
            .doc("Configuration file path. The default configuration file path is " + config_path),
        (option("-p", "--port") & value("port", port))
            .doc("Port to use for connection. The default connection port is 38709"),
        option("-q", "--quiet").set(quiet).doc("Silence outputs"),
        option("-h", "--help").set(help).doc("Print this help information on this tool and exit"));

    if (!parse(argc, argv, cli)) {
        std::cerr << make_man_page(cli, argv[0]);
        exit(1);
    }
    if (help) {
        std::cout << make_man_page(cli, argv[0]);
        exit(0);
    }

    // Clearing the config_path input argument
    if (!config_path.empty()) {
        if (config_path.front() == '=' || config_path.front() == ' ')
            config_path = config_path.substr(1, config_path.length() - 1);
    }

    std::string config {};
    stdfs::file_status config_status = stdfs::status(config_path);
    if (config_status.type() == stdfs::file_type::not_found) {
        stdfs::create_directories("./tmp/ustore/");
        config.assign(R"({
        "version": "1.0",
        "directory": "./tmp/ustore/",
        "data_directories": [],
        "engine": {
            "config_url": "",
            "config_file_path": "",
            "config": {}
        }})");
    }
    else {
        std::ifstream ifs(config_path);
        config = std::string((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    }

    return run_server(config.c_str(), port, quiet).ok() ? EXIT_SUCCESS : EXIT_FAILURE;
}

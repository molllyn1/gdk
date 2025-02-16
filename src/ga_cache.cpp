#include <algorithm>
#include <array>
#include <fstream>
#include <vector>

#include "assertion.hpp"
#include "ga_cache.hpp"
#include "logging.hpp"
#include "network_parameters.hpp"
#include "session.hpp"
#include "sqlite3/sqlite3.h"
#include "utils.hpp"

namespace ga {
namespace sdk {

    namespace {

        constexpr int VERSION = 1;
        constexpr const char* KV_SELECT = "SELECT value FROM KeyValue WHERE key = ?1;";

        static cache::sqlite3_ptr get_new_memory_db()
        {
            sqlite3* tmpdb = nullptr;
            const int rc = sqlite3_open(":memory:", &tmpdb);
            GDK_RUNTIME_ASSERT(rc == SQLITE_OK);
            return cache::sqlite3_ptr{ tmpdb, [](sqlite3* p) { sqlite3_free(p); } };
        }

        static auto get_db()
        {
            // Verify thread safety in the event that sqlite has been upgraded
            GDK_RUNTIME_ASSERT(sqlite3_threadsafe());

            auto db = get_new_memory_db();
            const auto exec_check = [&db](const char* sql) {
                char* err_msg = nullptr;
                const int rc = sqlite3_exec(db.get(), sql, 0, 0, &err_msg);
                if (rc != SQLITE_OK) {
                    GDK_LOG_SEV(log_level::info) << "Bad exec_check RC " << rc << " err_msg: " << err_msg;
                    GDK_RUNTIME_ASSERT(false);
                }
            };
            exec_check("CREATE TABLE LiquidOutput(txid BLOB NOT NULL, vout INTEGER NOT NULL, assetid BLOB NOT NULL, "
                       "satoshi INTEGER NOT NULL, abf BLOB NOT NULL, vbf BLOB NOT NULL, PRIMARY KEY (txid, vout));");

            exec_check("CREATE TABLE KeyValue(key BLOB NOT NULL, value BLOB NOT NULL, PRIMARY KEY(key));");

            exec_check("CREATE TABLE LiquidBlindingNonce(pubkey BLOB NOT NULL, script BLOB NOT NULL, nonce BLOB NOT "
                       "NULL, PRIMARY KEY(pubkey, script));");

            return db;
        }

        static cache::sqlite3_stmt_ptr get_stmt(bool enable, cache::sqlite3_ptr& db, const char* statement)
        {
            sqlite3_stmt* stmt = nullptr;
            if (enable) {
                int rc = sqlite3_prepare_v3(db.get(), statement, -1, SQLITE_PREPARE_PERSISTENT, &stmt, NULL);
                GDK_RUNTIME_ASSERT_MSG(rc == SQLITE_OK, sqlite3_errmsg(db.get()));
            }
            return cache::sqlite3_stmt_ptr{ stmt, [](sqlite3_stmt* p) { sqlite3_finalize(p); } };
        }

        static void db_log_error(sqlite3* db) noexcept
        {
            try {
                GDK_LOG_SEV(log_level::error) << "DB error: " << sqlite3_errmsg(db);
            } catch (const std::exception&) {
            }
        }

        static void stmt_check_clean(cache::sqlite3_stmt_ptr& stmt) noexcept
        {
            if (sqlite3_clear_bindings(stmt.get()) != SQLITE_OK) {
                db_log_error(sqlite3_db_handle(stmt.get()));
            }
            if (sqlite3_reset(stmt.get()) != SQLITE_OK) {
                db_log_error(sqlite3_db_handle(stmt.get()));
            }
        }

        static auto stmt_clean(cache::sqlite3_stmt_ptr& stmt)
        {
            return gsl::finally([&stmt] { stmt_check_clean(stmt); });
        }

        static void save_db_file(byte_span_t key, byte_span_t data, const std::string& path)
        {
            GDK_RUNTIME_ASSERT(!key.empty() && !data.empty());
            std::ofstream f(path, f.out | f.binary);
            if (f.is_open()) {
                const size_t encrypted_len = aes_gcm_encrypt_get_length(data);
                std::vector<unsigned char> cyphertext(encrypted_len);
                GDK_RUNTIME_ASSERT(aes_gcm_encrypt(key, data, cyphertext) == encrypted_len);

                for (size_t written = 0; written != encrypted_len; written = f.tellp()) {
                    auto p = reinterpret_cast<const char*>(&cyphertext[written]);
                    f.write(p, encrypted_len - written);
                }
            }
        }

        static std::vector<unsigned char> load_db_file(byte_span_t key, const std::string& path)
        {
            GDK_RUNTIME_ASSERT(!key.empty());
            std::ifstream f(path, f.in | f.binary);
            if (!f.is_open()) {
                GDK_LOG_SEV(log_level::info) << "Load db, no file or bad file " << path;
                return std::vector<unsigned char>();
            }

            f.seekg(0, f.end);
            std::vector<unsigned char> cyphertext(f.tellg());
            f.seekg(0, f.beg);

            size_t read = 0;
            while (read != cyphertext.size()) {
                auto p = reinterpret_cast<char*>(&cyphertext[read]);
                f.read(p, cyphertext.size() - read);
                read += f.gcount();
            }

            const size_t decrypted_len = aes_gcm_decrypt_get_length(cyphertext);
            std::vector<unsigned char> plaintext(decrypted_len);
            GDK_RUNTIME_ASSERT(aes_gcm_decrypt(key, cyphertext, plaintext) == decrypted_len);
            return plaintext;
        }

        static std::string get_persistent_storage_file(
            const std::string& data_dir, const std::string& db_name, int version)
        {
            return data_dir + '/' + std::to_string(version) + db_name + ".sqliteaesgcm";
        }

        static void clean_up_old_db(const std::string& data_dir, const std::string& db_name)
        {
            for (int version = 0; version < VERSION; ++version) {
                const auto path = get_persistent_storage_file(data_dir, db_name, version);
                {
                    const std::ifstream f(path, f.in | f.binary);
                    if (!f.is_open()) {
                        continue;
                    }
                }
                GDK_LOG_SEV(log_level::info) << "Deleting old version " << version << " db file " << path;
                unlink(path.c_str());
            }
        }

        static bool load_db_impl(byte_span_t key, const std::string& path, cache::sqlite3_ptr& db)
        {
            std::vector<unsigned char> plaintext;
            try {
                plaintext = load_db_file(key, path);
            } catch (const std::exception& ex) {
                GDK_LOG_SEV(log_level::info) << "Bad decryption for file " << path << " error " << ex.what();
                unlink(path.c_str());
            }

            if (plaintext.empty()) {
                return false;
            }

            const auto tmpdb = get_new_memory_db();
            const int rc = sqlite3_deserialize(
                tmpdb.get(), "main", plaintext.data(), plaintext.size(), plaintext.size(), SQLITE_DESERIALIZE_READONLY);

            if (rc != SQLITE_OK) {
                GDK_LOG_SEV(log_level::info) << "Bad sqlite3_deserialize for file " << path << " RC " << rc;
                unlink(path.c_str());
                return false;
            }

            auto backup = sqlite3_backup_init(db.get(), "main", tmpdb.get(), "main");
            bool ok = backup != nullptr && sqlite3_backup_step(backup, -1) == SQLITE_DONE;
            ok = ok && sqlite3_backup_finish(backup) == SQLITE_OK;
            if (!ok) {
                db_log_error(db.get());
                return false;
            }
            GDK_LOG_SEV(log_level::info) << path << " loaded correctly";
            return true;
        }

        static auto step_final(cache::sqlite3_stmt_ptr& stmt)
        {
            GDK_RUNTIME_ASSERT(sqlite3_step(stmt.get()) == SQLITE_DONE);
        }

        static boost::optional<std::vector<unsigned char>> get_blob(cache::sqlite3_stmt_ptr& stmt, int column)
        {
            const int rc = sqlite3_step(stmt.get());
            if (rc == SQLITE_DONE) {
                return boost::none;
            }
            GDK_RUNTIME_ASSERT(rc == SQLITE_ROW);

            const auto res = reinterpret_cast<const unsigned char*>(sqlite3_column_blob(stmt.get(), column));
            const auto len = sqlite3_column_bytes(stmt.get(), column);
            std::vector<unsigned char> result(res, res + len);
            step_final(stmt);
            return result;
        }

        static void get_blob(cache::sqlite3_stmt_ptr& stmt, int column, const cache::get_key_value_fn& callback)
        {
            const int rc = sqlite3_step(stmt.get());
            if (rc == SQLITE_DONE) {
                callback(boost::none);
                return;
            }
            GDK_RUNTIME_ASSERT(rc == SQLITE_ROW);

            const auto res = reinterpret_cast<const unsigned char*>(sqlite3_column_blob(stmt.get(), column));
            const auto len = sqlite3_column_bytes(stmt.get(), column);
            try {
                callback(gsl::make_span(res, len));
            } catch (const std::exception& ex) {
                GDK_LOG_SEV(log_level::error) << "Blob callback exception: " << ex.what();
            }
            step_final(stmt);
        }

        static void bind_blob(cache::sqlite3_stmt_ptr& stmt, int column, byte_span_t blob)
        {
            GDK_RUNTIME_ASSERT(
                sqlite3_bind_blob(stmt.get(), column, blob.data(), blob.size(), SQLITE_STATIC) == SQLITE_OK);
        }

        static void bind_liquid_blinding(cache::sqlite3_stmt_ptr& stmt, byte_span_t pubkey, byte_span_t script)
        {
            bind_blob(stmt, 1, pubkey);
            bind_blob(stmt, 2, script);
        }

        static bool has_result(cache::sqlite3_stmt_ptr& stmt)
        {
            const int rc = sqlite3_step(stmt.get());
            if (rc == SQLITE_DONE) {
                return false;
            }
            GDK_RUNTIME_ASSERT(rc == SQLITE_ROW);
            step_final(stmt);
            return true;
        }
    } // namespace

    cache::cache(const network_parameters& net_params, const std::string& network_name)
        : m_network_name(network_name)
        , m_is_liquid(net_params.is_liquid())
        , m_type(0)
        , m_data_dir()
        , m_db_name()
        , m_encryption_key()
        , m_require_write(false)
        , m_db(get_db())
        , m_stmt_liquid_blinding_nonce_has(get_stmt(
              m_is_liquid, m_db, "SELECT 1 FROM LiquidBlindingNonce WHERE pubkey = ?1 AND script = ?2 LIMIT 1;"))
        , m_stmt_liquid_blinding_nonce_search(
              get_stmt(m_is_liquid, m_db, "SELECT nonce FROM LiquidBlindingNonce WHERE pubkey = ?1 AND script = ?2;"))
        , m_stmt_liquid_blinding_nonce_insert(get_stmt(
              m_is_liquid, m_db, "INSERT INTO LiquidBlindingNonce (pubkey, script, nonce) VALUES (?1, ?2, ?3);"))
        , m_stmt_liquid_output_has(
              get_stmt(m_is_liquid, m_db, "SELECT 1 FROM LiquidOutput WHERE txid = ?1 AND vout = ?2 LIMIT 1;"))
        , m_stmt_liquid_output_search(get_stmt(
              m_is_liquid, m_db, "SELECT assetid, satoshi, abf, vbf FROM LiquidOutput WHERE txid = ?1 AND vout = ?2;"))
        , m_stmt_liquid_output_insert(get_stmt(m_is_liquid, m_db,
              "INSERT INTO LiquidOutput (txid, vout, assetid, satoshi, abf, vbf) VALUES (?1, ?2, ?3, ?4, ?5, ?6);"))
        , m_stmt_key_value_upsert(get_stmt(
              true, m_db, "INSERT INTO KeyValue(key, value) VALUES (?1, ?2) ON CONFLICT(key) DO UPDATE SET value=?2;"))
        , m_stmt_key_value_search(get_stmt(true, m_db, KV_SELECT))
        , m_stmt_key_value_delete(get_stmt(true, m_db, "DELETE FROM KeyValue WHERE key = ?1;"))
    {
    }

    void cache::save_db()
    {
        if (m_db_name.empty() || !m_require_write) {
            return;
        }
        sqlite3_int64 db_size;
        void* db = sqlite3_serialize(m_db.get(), "main", &db_size, 0);
        const auto _stmt_clean = gsl::finally([&db] { sqlite3_free(db); });
        if (db == nullptr || db_size < 1) {
            return;
        }
        const auto data = gsl::make_span(reinterpret_cast<const unsigned char*>(db), db_size);
        const auto path = get_persistent_storage_file(m_data_dir, m_db_name, VERSION);
        save_db_file(m_encryption_key, data, path);
        m_require_write = false;
    }

    void cache::load_db(byte_span_t encryption_key, const uint32_t type)
    {
        GDK_RUNTIME_ASSERT(!encryption_key.empty());

        m_data_dir = gdk_config().value("datadir", std::string{});
        if (m_data_dir.empty()) {
            GDK_LOG_SEV(log_level::info) << "datadir not set - thus no get_persistent_storage_file available";
            return;
        }

        m_type = type;
        const auto intermediate = hmac_sha512(encryption_key, ustring_span(m_network_name));
        // Note: the line below means the file name is endian dependant
        const auto type_span = gsl::make_span(reinterpret_cast<const unsigned char*>(&type), sizeof(m_type));
        m_db_name = b2h(gsl::make_span(hmac_sha512(intermediate, type_span).data(), 16));
        m_encryption_key = sha256(encryption_key);

        const auto path = get_persistent_storage_file(m_data_dir, m_db_name, VERSION);
        if (!load_db_impl(m_encryption_key, path, m_db)) {
            // Failed to load the latest version.
            if (VERSION > 1) {
                // Try to carry forward our client blob from the previous version
                try {
                    const auto prev_path = get_persistent_storage_file(m_data_dir, m_db_name, VERSION - 1);
                    auto db{ get_db() };
                    if (load_db_impl(m_encryption_key, prev_path, db)) {
                        auto stmt{ get_stmt(true, db, KV_SELECT) };
                        const auto _{ stmt_clean(stmt) };
                        const char* blob_key = "client_blob";
                        bind_blob(stmt, 1, ustring_span(blob_key));
                        auto prev_blob = get_blob(stmt, 0);
                        if (prev_blob) {
                            upsert_key_value(blob_key, prev_blob.get());
                        }
                        GDK_LOG_SEV(log_level::info) << "Copied client blob from previous version";
                    }
                } catch (const std::exception&) {
                    // Ignore errors; fetch blob from server or recreate instead
                }
            }

            // Clean up old versions only on initial DB creation
            clean_up_old_db(m_data_dir, m_db_name);
        }
    }

    void cache::clear_key_value(const std::string& key)
    {
        GDK_RUNTIME_ASSERT(!key.empty());
        const auto _{ stmt_clean(m_stmt_key_value_delete) };
        const auto key_span = ustring_span(key);
        bind_blob(m_stmt_key_value_delete, 1, key_span);
        step_final(m_stmt_key_value_delete);
    }

    void cache::get_key_value(const std::string& key, const cache::get_key_value_fn& callback)
    {
        GDK_RUNTIME_ASSERT(!key.empty());
        const auto _{ stmt_clean(m_stmt_key_value_search) };
        const auto key_span = ustring_span(key);
        bind_blob(m_stmt_key_value_search, 1, key_span);
        get_blob(m_stmt_key_value_search, 0, callback);
    }

    bool cache::has_liquid_blinding_nonce(byte_span_t pubkey, byte_span_t script)
    {
        GDK_RUNTIME_ASSERT(!pubkey.empty() && !script.empty());
        if (!m_stmt_liquid_blinding_nonce_has) {
            return false;
        }
        const auto _{ stmt_clean(m_stmt_liquid_blinding_nonce_has) };
        bind_liquid_blinding(m_stmt_liquid_blinding_nonce_has, pubkey, script);
        return has_result(m_stmt_liquid_blinding_nonce_has);
    }

    boost::optional<std::vector<unsigned char>> cache::get_liquid_blinding_nonce(byte_span_t pubkey, byte_span_t script)
    {
        GDK_RUNTIME_ASSERT(!pubkey.empty() && !script.empty());
        GDK_RUNTIME_ASSERT(m_stmt_liquid_blinding_nonce_search.get());
        const auto _{ stmt_clean(m_stmt_liquid_blinding_nonce_search) };
        bind_liquid_blinding(m_stmt_liquid_blinding_nonce_search, pubkey, script);
        return get_blob(m_stmt_liquid_blinding_nonce_search, 0);
    }

    bool cache::has_liquid_output(byte_span_t txhash, const uint32_t vout)
    {
        GDK_RUNTIME_ASSERT(!txhash.empty());
        if (!m_stmt_liquid_blinding_nonce_has) {
            return false;
        }
        const auto _{ stmt_clean(m_stmt_liquid_output_has) };
        bind_blob(m_stmt_liquid_output_has, 1, txhash);
        GDK_RUNTIME_ASSERT(sqlite3_bind_int(m_stmt_liquid_output_has.get(), 2, vout) == SQLITE_OK);
        return has_result(m_stmt_liquid_output_has);
    }

    boost::optional<nlohmann::json> cache::get_liquid_output(byte_span_t txhash, const uint32_t vout)
    {
        GDK_RUNTIME_ASSERT(!txhash.empty());
        GDK_RUNTIME_ASSERT(m_stmt_liquid_output_search.get());
        const auto _{ stmt_clean(m_stmt_liquid_output_search) };
        bind_blob(m_stmt_liquid_output_search, 1, txhash);
        GDK_RUNTIME_ASSERT(sqlite3_bind_int(m_stmt_liquid_output_search.get(), 2, vout) == SQLITE_OK);
        const int rc = sqlite3_step(m_stmt_liquid_output_search.get());
        if (rc == SQLITE_DONE) {
            return boost::none;
        }
        GDK_RUNTIME_ASSERT(rc == SQLITE_ROW);
        nlohmann::json utxo;
        const auto _get_reversed_result = [this](const int column, const int size) {
            GDK_RUNTIME_ASSERT(sqlite3_column_bytes(m_stmt_liquid_output_search.get(), column) == size);
            const auto res = reinterpret_cast<const unsigned char*>(
                sqlite3_column_blob(m_stmt_liquid_output_search.get(), column));
            GDK_RUNTIME_ASSERT(res);
            // cache values are stored in byte order not display order (reversed)
            return b2h_rev(gsl::make_span(res, size));
        };

        utxo["asset_id"] = _get_reversed_result(0, ASSET_TAG_LEN);
        utxo["satoshi"] = sqlite3_column_int64(m_stmt_liquid_output_search.get(), 1);
        utxo["assetblinder"] = _get_reversed_result(2, 32);
        utxo["amountblinder"] = _get_reversed_result(3, 32);

        step_final(m_stmt_liquid_output_search);
        return utxo;
    }

    void cache::upsert_key_value(const std::string& key, byte_span_t value)
    {
        GDK_RUNTIME_ASSERT(!key.empty() && !value.empty());
        const auto _{ stmt_clean(m_stmt_key_value_upsert) };
        const auto key_span = ustring_span(key);
        bind_blob(m_stmt_key_value_upsert, 1, key_span);
        bind_blob(m_stmt_key_value_upsert, 2, value);
        step_final(m_stmt_key_value_upsert);
        m_require_write = true;
    }

    void cache::insert_liquid_blinding_nonce(byte_span_t pubkey, byte_span_t script, byte_span_t nonce)
    {
        GDK_RUNTIME_ASSERT(!pubkey.empty() && !script.empty() && !nonce.empty());
        GDK_RUNTIME_ASSERT(m_stmt_liquid_blinding_nonce_insert.get());
        const auto _{ stmt_clean(m_stmt_liquid_blinding_nonce_insert) };
        bind_liquid_blinding(m_stmt_liquid_blinding_nonce_insert, pubkey, script);
        bind_blob(m_stmt_liquid_blinding_nonce_insert, 3, nonce);
        step_final(m_stmt_liquid_blinding_nonce_insert);
        m_require_write = true;
    }

    void cache::insert_liquid_output(byte_span_t txhash, uint32_t vout, nlohmann::json& utxo)
    {
        GDK_RUNTIME_ASSERT(!txhash.empty() && !utxo.empty());
        GDK_RUNTIME_ASSERT(m_stmt_liquid_output_insert.get());
        const auto _{ stmt_clean(m_stmt_liquid_output_insert) };

        // cache values are stored in byte order not display order (reversed)
        bind_blob(m_stmt_liquid_output_insert, 1, txhash);

        GDK_RUNTIME_ASSERT(sqlite3_bind_int(m_stmt_liquid_output_insert.get(), 2, vout) == SQLITE_OK);
        const auto assetid = h2b_rev(utxo["asset_id"]);
        bind_blob(m_stmt_liquid_output_insert, 3, assetid);

        const auto satoshi = utxo.at("satoshi");
        GDK_RUNTIME_ASSERT(sqlite3_bind_int64(m_stmt_liquid_output_insert.get(), 4, satoshi) == SQLITE_OK);

        const auto abf = h2b_rev(utxo["assetblinder"]);
        bind_blob(m_stmt_liquid_output_insert, 5, abf);
        const auto vbf = h2b_rev(utxo["amountblinder"]);
        bind_blob(m_stmt_liquid_output_insert, 6, vbf);

        step_final(m_stmt_liquid_output_insert);
        m_require_write = true;
    }
} // namespace sdk
} // namespace ga

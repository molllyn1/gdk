#ifndef GDK_GA_AUTH_HANDLERS_HPP
#define GDK_GA_AUTH_HANDLERS_HPP
#pragma once

#include "auth_handler.hpp"

namespace ga {
namespace sdk {
    class register_call : public auth_handler {
    public:
        register_call(session& session, const nlohmann::json& hw_device, const std::string& mnemonic);

    private:
        state_type call_impl() override;

        std::string m_mnemonic;
    };

    // Return an auth handler for logging in a user (caller must delete it)
    auth_handler* get_login_call(
        session& session, const nlohmann::json& hw_device, const nlohmann::json& credential_data);

    class login_call : public auth_handler {
    public:
        login_call(session& session, const nlohmann::json& hw_device, const std::string& mnemonic,
            const std::string& password);

    private:
        state_type call_impl() override;

        std::string m_challenge;
        std::string m_mnemonic;
        std::string m_master_xpub_bip32;
        std::string m_password;
        bool m_use_ae_protocol;

        // used for 2of2_no_recovery
        std::unordered_map<uint32_t, std::vector<std::string>> m_ca_addrs;
        std::vector<uint32_t> m_ca_reqs;
    };

    class login_with_pin_call : public auth_handler {
    public:
        login_with_pin_call(session& session, const std::string& pin, const nlohmann::json& pin_data);

    private:
        state_type call_impl() override;

        std::string m_pin;
        nlohmann::json m_pin_data;
    };

    class watch_only_login_call : public auth_handler {
    public:
        watch_only_login_call(session& session, const nlohmann::json& credential_data);

    private:
        state_type call_impl() override;

        nlohmann::json m_credential_data;
    };

    class create_subaccount_call : public auth_handler {
    public:
        create_subaccount_call(session& session, const nlohmann::json& details);

    private:
        state_type call_impl() override;

        nlohmann::json m_details;
        uint32_t m_subaccount;

        // used by 2of3 subaccounts
        std::string m_master_xpub_bip32;
        std::string m_subaccount_xpub;
        bool m_use_ae_protocol;

        // used when a new authorized-assets-enabled subaccount is created
        std::vector<std::string> m_ca_addrs;
        uint32_t m_remaining_ca_addrs;
    };

    class ack_system_message_call : public auth_handler {
    public:
        ack_system_message_call(session& session, const std::string& msg);

    private:
        state_type call_impl() override;

        std::string m_message;
        std::pair<std::string, std::vector<uint32_t>> m_message_info;

        bool m_use_ae_protocol;
        std::string m_master_xpub_bip32;
    };

    class sign_transaction_call : public auth_handler {
    public:
        sign_transaction_call(session& session, const nlohmann::json& tx_details);

    private:
        state_type call_impl() override;

        nlohmann::json m_tx_details;
        bool m_use_ae_protocol;
    };

    class get_receive_address_call : public auth_handler {
    public:
        get_receive_address_call(session& session, const nlohmann::json& details);

    private:
        state_type call_impl() override;

        const nlohmann::json m_details;
    };

    class get_previous_addresses_call : public auth_handler {
    public:
        get_previous_addresses_call(session& session, const nlohmann::json& details);

    private:
        state_type call_impl() override;
        state_type set_address_to_blind();

        const nlohmann::json m_details;
        size_t m_index;
    };

    class create_transaction_call : public auth_handler {
    public:
        create_transaction_call(session& session, const nlohmann::json& details);

    private:
        state_type call_impl() override;

        const nlohmann::json m_details;
        nlohmann::json m_tx;
    };

    class needs_unblind_call : public auth_handler {
    protected:
        needs_unblind_call(const std::string& name, session& session, const nlohmann::json& details);

        virtual state_type wrapped_call_impl() = 0;

        const nlohmann::json m_details;

    private:
        state_type call_impl() override;

        const liquid_support_level m_liquid_support;
    };

    class get_balance_call : public needs_unblind_call {
    public:
        get_balance_call(session& session, const nlohmann::json& details);

    private:
        state_type wrapped_call_impl() override;
    };

    class get_subaccounts_call : public needs_unblind_call {
    public:
        get_subaccounts_call(session& session);

    private:
        state_type wrapped_call_impl() override;
    };

    class get_subaccount_call : public needs_unblind_call {
    public:
        get_subaccount_call(session& session, uint32_t subaccount);

    private:
        state_type wrapped_call_impl() override;
    };

    class get_transactions_call : public needs_unblind_call {
    public:
        get_transactions_call(session& session, const nlohmann::json& details);

    private:
        state_type wrapped_call_impl() override;
    };

    class get_unspent_outputs_call : public needs_unblind_call {
    public:
        get_unspent_outputs_call(session& session, const nlohmann::json& details);

    private:
        state_type wrapped_call_impl() override;
    };

    class set_unspent_outputs_status_call : public auth_handler {
    public:
        set_unspent_outputs_status_call(session& session, const nlohmann::json& details);

    private:
        state_type call_impl() override;

        nlohmann::json m_details;
    };

    class get_expired_deposits_call : public needs_unblind_call {
    public:
        get_expired_deposits_call(session& session, const nlohmann::json& details);

    private:
        state_type wrapped_call_impl() override;
    };

    class change_settings_call : public auth_handler {
    public:
        change_settings_call(session& session, const nlohmann::json& settings);

    private:
        state_type call_impl() override;

        nlohmann::json m_settings;
        nlohmann::json m_nlocktime_value;
    };

    class change_settings_twofactor_call : public auth_handler {
    public:
        change_settings_twofactor_call(
            session& session, const std::string& method_to_update, const nlohmann::json& details);

    private:
        state_type call_impl() override;

        state_type on_init_done(const std::string& new_action);

        nlohmann::json m_current_config;
        std::string m_method_to_update;
        nlohmann::json m_details;
        nlohmann::json m_gauth_data;
        bool m_enabling;
    };

    class update_subaccount_call : public auth_handler {
    public:
        update_subaccount_call(session& session, const nlohmann::json& details);

    private:
        state_type call_impl() override;

        const nlohmann::json m_details;
    };

    class change_limits_call : public auth_handler {
    public:
        change_limits_call(session& session, const nlohmann::json& details);

        void request_code(const std::string& method) override;

    private:
        state_type call_impl() override;

        nlohmann::json m_limit_details;
        bool m_is_decrease;
    };

    class remove_account_call : public auth_handler {
    public:
        explicit remove_account_call(session& session);

    private:
        state_type call_impl() override;
    };

    class send_transaction_call final : public auth_handler {
    public:
        send_transaction_call(session& session, const nlohmann::json& tx_details);

        void request_code(const std::string& method) override;

    private:
        state_type call_impl() override;

        void create_twofactor_data();

        nlohmann::json m_tx_details;
        nlohmann::json m_limit_details;
        uint64_t m_bump_amount = 0;
        bool m_twofactor_required;
        bool m_under_limit;
    };

    class twofactor_reset_call : public auth_handler {
    public:
        twofactor_reset_call(session& session, const std::string& email, bool is_dispute, bool is_undo);

    private:
        state_type call_impl() override;

        const std::string m_reset_email;
        const bool m_is_dispute;
        const bool m_is_undo;
        bool m_confirming;
    };

    class twofactor_cancel_reset_call final : public auth_handler {
    public:
        explicit twofactor_cancel_reset_call(session& session);

    private:
        state_type call_impl() override;
    };

    class csv_time_call : public auth_handler {
    public:
        explicit csv_time_call(session& session, const nlohmann::json& params);

    private:
        state_type call_impl() override;

        nlohmann::json m_params;
    };

    class nlocktime_call : public auth_handler {
    public:
        explicit nlocktime_call(session& session, const nlohmann::json& params);

    private:
        state_type call_impl() override;

        nlohmann::json m_params;
    };
} // namespace sdk
} // namespace ga
#endif

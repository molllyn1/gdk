#include "session_impl.hpp"
#include "exception.hpp"
#include "logging.hpp"

namespace ga {
namespace sdk {

    namespace {
        static network_parameters get_network_overrides(const nlohmann::json& user_params)
        {
            // Get the registered network parameters the passed in parameters are based on
            auto ret = network_parameters::get(user_params.at("name"));
            // Set override-able settings from the users parameters
            ret["use_tor"] = user_params.value("use_tor", ret.value("use_tor", false));
            ret["user_agent"] = user_params.value("user_agent", ret.value("user_agent", std::string()));
            ret["electrum_url"] = user_params.value("electrum_url", ret.at("electrum_url"));
            ret["spv_enabled"] = user_params.value("spv_enabled", ret.at("spv_enabled"));
            ret["tls"] = user_params.value("tls", ret.at("tls"));

            return network_parameters{ ret };
        }
    } // namespace

    session_impl::session_impl(const nlohmann::json& net_params)
        : m_net_params(get_network_overrides(net_params))
    {
    }

    session_impl::~session_impl() {}

} // namespace sdk
} // namespace ga

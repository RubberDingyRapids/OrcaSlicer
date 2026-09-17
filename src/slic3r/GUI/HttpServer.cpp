#include "HttpServer.hpp"
#include <boost/log/trivial.hpp>
#include "GUI_App.hpp"
#include "slic3r/Utils/Http.hpp"
#include "slic3r/Utils/NetworkAgent.hpp"
#include "slic3r/Utils/BBLNetworkPlugin.hpp"
#include "slic3r/Utils/PJarczakLinuxBridge/PJarczakLinuxBridgeConfig.hpp"
#include "libslic3r/Thread.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <map>
#include <thread>
#include <chrono>

using json = nlohmann::json;

namespace Slic3r {
namespace GUI {

std::string url_get_param(const std::string& url, const std::string& key)
{
    size_t start = url.find(key);
    if (start == std::string::npos) return "";
    size_t eq = url.find('=', start);
    if (eq == std::string::npos) return "";
    std::string key_str = url.substr(start, eq - start);
    if (key_str != key)
        return "";
    start += key.size() + 1;
    size_t end = url.find('&', start);
    if (end == std::string::npos) end = url.length(); // Last param
    std::string result = url.substr(start, end - start);
    return result;
}

// Debug aid: report which *class* of token the auth flow minted, without ever
// writing the token itself to the log. A genuine Bambu session token is a JWT
// (three base64url parts) whose claims name the audience/scope it is valid for;
// an opaque token of a fixed length is a different grant entirely.
static std::string b64url_decode(const std::string& in)
{
    static const std::string tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    int val = 0, bits = -8;
    for (unsigned char c : in) {
        const size_t idx = tbl.find(static_cast<char>(c));
        if (idx == std::string::npos) continue;
        val = (val << 6) + static_cast<int>(idx);
        bits += 6;
        if (bits >= 0) {
            out.push_back(static_cast<char>((val >> bits) & 0xFF));
            bits -= 8;
        }
    }
    return out;
}

static void log_token_shape(const char* label, const std::string& token)
{
    const size_t parts = static_cast<size_t>(std::count(token.begin(), token.end(), '.')) + 1;
    BOOST_LOG_TRIVIAL(info) << "token_shape[" << label << "]: len=" << token.size()
                            << ", parts=" << parts << ", jwt=" << (parts == 3 ? "yes" : "no");
    if (parts != 3)
        return;
    const size_t d1 = token.find('.');
    const size_t d2 = token.find('.', d1 + 1);
    BOOST_LOG_TRIVIAL(info) << "token_shape[" << label << "] header=" << b64url_decode(token.substr(0, d1));
    BOOST_LOG_TRIVIAL(info) << "token_shape[" << label << "] claims=" << b64url_decode(token.substr(d1 + 1, d2 - d1 - 1));
}

void session::start()
{
    read_first_line();
}

void session::stop()
{
    boost::system::error_code ignored_ec;
    socket.shutdown(boost::asio::socket_base::shutdown_both, ignored_ec);
    socket.close(ignored_ec);
}

void session::read_first_line()
{
    auto self(shared_from_this());

    async_read_until(socket, buff, '\r', [this, self](const boost::beast::error_code& e, std::size_t s) {
        if (!e) {
            std::string  line, ignore;
            std::istream stream{&buff};
            std::getline(stream, line, '\r');
            std::getline(stream, ignore, '\n');
            headers.on_read_request_line(line);
            read_next_line();
        } else if (e != boost::asio::error::operation_aborted) {
            server.stop(self);
        }
    });
}

void session::read_body()
{
    auto self(shared_from_this());

    int                                nbuffer = 1000;
    std::shared_ptr<std::vector<char>> bufptr  = std::make_shared<std::vector<char>>(nbuffer);
    async_read(socket, boost::asio::buffer(*bufptr, nbuffer),
               [this, self, bufptr](const boost::beast::error_code& e, std::size_t s) { server.stop(self); });
}

void session::read_next_line()
{
    auto self(shared_from_this());

    async_read_until(socket, buff, '\r', [this, self](const boost::beast::error_code& e, std::size_t s) {
        if (!e) {
            std::string  line, ignore;
            std::istream stream{&buff};
            std::getline(stream, line, '\r');
            std::getline(stream, ignore, '\n');
            headers.on_read_header(line);

            if (line.length() == 0) {
                if (headers.content_length() == 0) {
                    std::cout << "Request received: " << headers.method << " " << headers.get_url();
                    if (headers.method == "OPTIONS") {
                        // Ignore http OPTIONS
                        server.stop(self);
                        return;
                    }

                    const std::string url_str = Http::url_decode(headers.get_url());
                    const auto        resp    = server.server.m_request_handler(url_str);
                    std::stringstream ssOut;
                    resp->write_response(ssOut);
                    std::shared_ptr<std::string> str = std::make_shared<std::string>(ssOut.str());
                    async_write(socket, boost::asio::buffer(str->c_str(), str->length()),
                                [this, self, str](const boost::beast::error_code& e, std::size_t s) {
                        std::cout << "done" << std::endl;
                        server.stop(self);
                    });
                } else {
                    read_body();
                }
            } else {
                read_next_line();
            }
        } else if (e != boost::asio::error::operation_aborted) {
            server.stop(self);
        }
    });
}

void HttpServer::IOServer::do_accept()
{
    acceptor.async_accept([this](boost::system::error_code ec, boost::asio::ip::tcp::socket socket) {
        if (!acceptor.is_open()) {
            return;
        }

        if (!ec) {
            const auto ss = std::make_shared<session>(*this, std::move(socket));
            start(ss);
        }

        do_accept();
    });
}

void HttpServer::IOServer::start(std::shared_ptr<session> session)
{
    sessions.insert(session);
    session->start();
}

void HttpServer::IOServer::stop(std::shared_ptr<session> session)
{
    sessions.erase(session);
    session->stop();
}

void HttpServer::IOServer::stop_all()
{
    for (auto s : sessions) {
        s->stop();
    }
    sessions.clear();
}


HttpServer::HttpServer(boost::asio::ip::port_type port) : port(port) {}

HttpServer::~HttpServer()
{
    stop();
}

void HttpServer::start()
{
    if (start_http_server)
        return;

    BOOST_LOG_TRIVIAL(info) << "start_http_service...";
    server_             = std::make_unique<IOServer>(*this);
    IOServer* io_server = server_.get();
    start_http_server   = true;
    m_http_server_thread = create_thread([io_server] {
        set_current_thread_name("http_server");
        io_server->acceptor.listen();

        io_server->do_accept();

        io_server->io_service.run();
    });
}

void HttpServer::stop()
{
    start_http_server = false;
    if (server_) {
        IOServer* io_server = server_.get();
        boost::asio::post(io_server->io_service, [io_server] {
            boost::system::error_code ec;
            io_server->acceptor.cancel(ec);
            io_server->acceptor.close(ec);
            io_server->stop_all();
            io_server->io_service.stop();
        });
    }
    if (m_http_server_thread.joinable())
        m_http_server_thread.join();
    server_.reset();
}

void HttpServer::set_request_handler(const std::function<std::shared_ptr<Response>(const std::string&)>& request_handler)
{
    this->m_request_handler = request_handler;
}

std::shared_ptr<HttpServer::Response> HttpServer::bbl_auth_handle_request(const std::string& url)
{
    return auth_handle_request(url, BBL_CLOUD_PROVIDER);
}

std::shared_ptr<HttpServer::Response> HttpServer::auth_handle_request(const std::string& url, const std::string& provider)
{
    BOOST_LOG_TRIVIAL(info) << "thirdparty_login: get_response";

    const std::string auth_code = url_get_param(url, "code");
    if (!auth_code.empty()) {
        std::string state = url_get_param(url, "orca_state");
        if (state.empty()) {
            state = url_get_param(url, "state");  // fallback
        }
        NetworkAgent* agent = wxGetApp().getAgent();
        if (!agent) {
            return std::make_shared<ResponseNotFound>();
        }

        json payload;
        payload["command"] = "user_login";
        payload["data"]["code"] = auth_code;
        payload["data"]["state"] = state;

        agent->change_user(payload.dump(), provider);
        const bool login_ok = agent->is_user_login(provider);
        if (login_ok) {
            wxGetApp().request_user_login(1, provider);
            GUI::wxGetApp().CallAfter([] { wxGetApp().ShowUserLogin(false); });
        }

        const std::string title = login_ok ? "Authentication complete" : "Authentication failed";
        const std::string message = login_ok
            ? "You can return to OrcaSlicer. This window will close automatically."
            : "Something went wrong. Please return to OrcaSlicer and try again.";
        const std::string html =
            "<html><head><meta charset=\"utf-8\">"
            "<style>body{font-family:Arial,sans-serif;background:#f7f7f7;color:#222;margin:32px;}"
            "a.button{display:inline-block;padding:10px 16px;margin-top:12px;background:#0f8bff;color:#fff;text-decoration:none;border-radius:6px;}"
            "</style></head><body><div class=\"container\">"
            "<h2>" + title + "</h2>"
            "<p>" + message + "</p>"
            "<script>setTimeout(function(){try{window.close();}catch(e){}},1500);</script>"
            "</div></body></html>";
        return std::make_shared<ResponseHtml>(html);
    }

    if (boost::contains(url, "access_token")) {
        std::string   redirect_url           = url_get_param(url, "redirect_url");
        std::string   access_token           = url_get_param(url, "access_token");
        std::string   refresh_token          = url_get_param(url, "refresh_token");
        std::string   expires_in_str         = url_get_param(url, "expires_in");
        std::string   refresh_expires_in_str = url_get_param(url, "refresh_expires_in");
        BOOST_LOG_TRIVIAL(info) << "legacy_login: access_token branch taken";
        log_token_shape("legacy_access", access_token);
        log_token_shape("legacy_refresh", refresh_token);
        Slic3r::PJarczakLinuxBridge::set_session_token(access_token);
        NetworkAgent* agent                  = wxGetApp().getAgent();

        unsigned int http_code;
        std::string  http_body;
        int          result = agent->get_my_profile(access_token, &http_code, &http_body, provider);
        if (result == 0) {
            std::string user_id;
            std::string user_name;
            std::string user_account;
            std::string user_avatar;
            try {
                json user_j = json::parse(http_body);
                if (user_j.contains("uidStr"))
                    user_id = user_j["uidStr"].get<std::string>();
                if (user_j.contains("name"))
                    user_name = user_j["name"].get<std::string>();
                if (user_j.contains("avatar"))
                    user_avatar = user_j["avatar"].get<std::string>();
                if (user_j.contains("account"))
                    user_account = user_j["account"].get<std::string>();
            } catch (...) {
                ;
            }
            json j;
            j["data"]["refresh_token"]      = refresh_token;
            j["data"]["token"]              = access_token;
            j["data"]["expires_in"]         = expires_in_str;
            j["data"]["refresh_expires_in"] = refresh_expires_in_str;
            j["data"]["user"]["uid"]        = user_id;
            j["data"]["user"]["name"]       = user_name;
            j["data"]["user"]["account"]    = user_account;
            j["data"]["user"]["avatar"]     = user_avatar;
            agent->change_user(j.dump(), provider);
            if (agent->is_user_login(provider)) {
                wxGetApp().request_user_login(1, provider);
            }
            GUI::wxGetApp().CallAfter([] { wxGetApp().ShowUserLogin(false); });
            std::string location_str = (boost::format("%1%?result=success") % redirect_url).str();
            return std::make_shared<ResponseRedirect>(location_str);
        } else {
            std::string error_str    = "get_user_profile_error_" + std::to_string(result);
            std::string location_str = (boost::format("%1%?result=fail&error=%2%") % redirect_url % error_str).str();
            return std::make_shared<ResponseRedirect>(location_str);
        }
    }

    // Ticket-based redirect: Bambu Lab's auth server redirects here after a
    // third-party (Google) OAuth so that the access token never travels through
    // the URL. We exchange the ticket via the network plugin's get_my_token,
    // then run the same get_my_profile + change_user flow as access_token.
    // Skip entirely on legacy plugins missing bambu_network_get_my_token —
    // those clients pin X-BBL-Client-Version so the server stays on the legacy
    // ?access_token= redirect path and never sends ?ticket= here.
    const std::string ticket = url_get_param(url, "ticket");
    const std::string ticket_redirect_url = url_get_param(url, "redirect_url");
    if (!ticket.empty() && !ticket_redirect_url.empty() &&
        BBLNetworkPlugin::instance().get_get_my_token() != nullptr) {
        BOOST_LOG_TRIVIAL(info) << "thirdparty_login: ticket flow";
        NetworkAgent* agent = wxGetApp().getAgent();
        if (!agent) {
            std::string location_str = (boost::format("%1%?result=fail&error=no_agent") % ticket_redirect_url).str();
            return std::make_shared<ResponseRedirect>(location_str);
        }

        auto fail_redirect = [&ticket_redirect_url](const std::string& reason) {
            std::string location_str = (boost::format("%1%?result=fail&error=%2%") % ticket_redirect_url % reason).str();
            return std::make_shared<ResponseRedirect>(location_str);
        };

        unsigned int token_http_code = 0;
        std::string  token_body;
        int          token_result = agent->get_my_token(ticket, &token_http_code, &token_body, provider);
        if (token_result != 0) {
            BOOST_LOG_TRIVIAL(warning) << "thirdparty_login: get_my_token failed, http_code=" << token_http_code;
            return fail_redirect("get_my_token_error_" + std::to_string(token_result));
        }

        std::string access_token;
        std::string refresh_token;
        std::string expires_in_str;
        std::string refresh_expires_in_str;
        try {
            json token_j = json::parse(token_body);
            if (token_j.contains("accessToken"))
                access_token = token_j["accessToken"].get<std::string>();
            if (token_j.contains("refreshToken"))
                refresh_token = token_j["refreshToken"].get<std::string>();
            if (token_j.contains("expiresIn"))
                expires_in_str = std::to_string(token_j["expiresIn"].get<double>());
            if (token_j.contains("refreshExpiresIn"))
                refresh_expires_in_str = std::to_string(token_j["refreshExpiresIn"].get<double>());
        } catch (...) {
            return fail_redirect("token_parse_error");
        }

        if (access_token.empty()) {
            return fail_redirect("token_missing");
        }
        BOOST_LOG_TRIVIAL(info) << "thirdparty_login: token exchange ok, http_code=" << token_http_code
                                << ", access_token len=" << access_token.size()
                                << ", refresh_token len=" << refresh_token.size()
                                << ", expires_in=" << expires_in_str;
        log_token_shape("access", access_token);
        log_token_shape("refresh", refresh_token);
        Slic3r::PJarczakLinuxBridge::set_session_token(access_token);
        {
            // Probe the exact endpoint the send path dies on, using Orca's own HTTP
            // client with the freshly minted token. /user/print already answers 200
            // here; /user/project is what returns 401 and fails the send with -3010.
            // If our own call succeeds where the plug-in's fails, the token is fine
            // and the caller's identity is the problem - and vice versa.
            std::string  proj_body;
            unsigned int proj_status = 0;
            Http::get("https://api.bambulab.com/v1/iot-service/api/user/project")
                .header("Authorization", "Bearer " + access_token)
                .header("Accept", "application/json")
                .on_complete([&proj_body, &proj_status](std::string body, unsigned status) {
                    proj_body = std::move(body); proj_status = status; })
                .on_error([&proj_body, &proj_status](std::string body, std::string err, unsigned status) {
                    proj_body = body + " err=" + err; proj_status = status; })
                .perform_sync();
            BOOST_LOG_TRIVIAL(info) << "thirdparty_login: direct /user/project probe status=" << proj_status
                                    << ", body head=" << proj_body.substr(0, 200);
            // A POST is what the send path actually issues. If an authenticated
            // request with a malformed body comes back 400/422 the token is fine
            // and the plug-in's caller identity is at fault; a 401 here means the
            // token itself is not accepted for writes.
            std::string  post_body;
            unsigned int post_status = 0;
            Http::post("https://api.bambulab.com/v1/iot-service/api/user/project")
                .header("Authorization", "Bearer " + access_token)
                .header("Content-Type", "application/json")
                .header("Accept", "application/json")
                .set_post_body(std::string("{}"))
                .on_complete([&post_body, &post_status](std::string body, unsigned status) {
                    post_body = std::move(body); post_status = status; })
                .on_error([&post_body, &post_status](std::string body, std::string err, unsigned status) {
                    post_body = body + " err=" + err; post_status = status; })
                .perform_sync();
            BOOST_LOG_TRIVIAL(info) << "thirdparty_login: direct POST /user/project probe status=" << post_status
                                    << ", body head=" << post_body.substr(0, 200);
            // The plug-in sends X-BBL-* identity headers on every request and gets 401
            // on writes, while a bare Bearer request authenticates (400). Two of those
            // headers contradict themselves: the client version does not match the
            // plug-in's own reported version, and OS-Type is forced to linux while
            // OS-Version carries a Windows build. Find out which one the server minds.
            auto probe_headers = [&access_token](const char* label,
                                                 const std::map<std::string, std::string>& hdrs) {
                std::string  b;
                unsigned int st = 0;
                auto req = Http::post("https://api.bambulab.com/v1/iot-service/api/user/project");
                req.header("Authorization", "Bearer " + access_token)
                   .header("Content-Type", "application/json")
                   .header("Accept", "application/json");
                for (const auto& kv : hdrs)
                    req.header(kv.first, kv.second);
                req.set_post_body(std::string("{}"))
                   .on_complete([&b, &st](std::string body, unsigned status) { b = std::move(body); st = status; })
                   .on_error([&b, &st](std::string body, std::string err, unsigned status) { b = body + " err=" + err; st = status; })
                   .perform_sync();
                BOOST_LOG_TRIVIAL(info) << "hdrprobe[" << label << "] status=" << st
                                        << " body=" << b.substr(0, 90);
            };

            const std::map<std::string, std::string> full = {
                {"X-BBL-Client-Name", "BambuStudio"},
                {"X-BBL-Client-Type", "slicer"},
                {"X-BBL-Client-Version", "02.08.01.55"},
                {"X-BBL-Device-ID", "b5efa233-1d15-4161-896d-81a3628e9ab3"},
                {"X-BBL-Language", "en-US"},
                {"X-BBL-OS-Type", "linux"},
                {"X-BBL-OS-Version", "10.0.19045"},
            };
            probe_headers("bare", {});
            probe_headers("full_as_plugin_sends", full);
            auto win_os = full; win_os["X-BBL-OS-Type"] = "windows";
            probe_headers("os_type_windows", win_os);
            auto ver53 = full; ver53["X-BBL-Client-Version"] = "02.08.01.53";
            probe_headers("client_version_02.08.01.53", ver53);
            auto no_os = full; no_os.erase("X-BBL-OS-Type"); no_os.erase("X-BBL-OS-Version");
            probe_headers("no_os_headers", no_os);
            auto only_ver = std::map<std::string, std::string>{{"X-BBL-Client-Version", "02.08.01.55"}};
            probe_headers("only_client_version", only_ver);
            auto only_os = std::map<std::string, std::string>{{"X-BBL-OS-Type", "linux"}};
            probe_headers("only_os_type_linux", only_os);
            // Identical requests come back 400 or 401 at random, so neither headers nor
            // version matter. Two explanations remain: we are being rate limited, or the
            // token is not honoured by every backend behind the load balancer. Spacing
            // the requests separates them - a rate limit eases off when we slow down, a
            // backend that has never seen the token does not care about timing.
            {
                std::string burst;
                for (int i = 0; i < 10; ++i) {
                    std::string  b;
                    unsigned int st = 0;
                    Http::post("https://api.bambulab.com/v1/iot-service/api/user/project")
                        .header("Authorization", "Bearer " + access_token)
                        .header("Content-Type", "application/json")
                        .set_post_body(std::string("{}"))
                        .on_complete([&b, &st](std::string body, unsigned status) { b = std::move(body); st = status; })
                        .on_error([&b, &st](std::string body, std::string err, unsigned status) { b = body; st = status; })
                        .perform_sync();
                    burst += std::to_string(st) + " ";
                }
                BOOST_LOG_TRIVIAL(info) << "ratelimit[burst_no_delay] " << burst;

                std::string spaced;
                for (int i = 0; i < 5; ++i) {
                    std::this_thread::sleep_for(std::chrono::seconds(3));
                    std::string  b;
                    unsigned int st = 0;
                    Http::post("https://api.bambulab.com/v1/iot-service/api/user/project")
                        .header("Authorization", "Bearer " + access_token)
                        .header("Content-Type", "application/json")
                        .set_post_body(std::string("{}"))
                        .on_complete([&b, &st](std::string body, unsigned status) { b = std::move(body); st = status; })
                        .on_error([&b, &st](std::string body, std::string err, unsigned status) { b = body; st = status; })
                        .perform_sync();
                    spaced += std::to_string(st) + " ";
                }
                BOOST_LOG_TRIVIAL(info) << "ratelimit[spaced_3s] " << spaced;
            }
            // The ticket exchange yields an opaque grant that the IoT service accepts
            // for reads but refuses for writes. Bambu's refresh endpoint is what the
            // official client uses to obtain a full session JWT; if it honours this
            // refresh token we can simply trade up and use the result everywhere.
            std::string  ref_body;
            unsigned int ref_status = 0;
            json         ref_req;
            ref_req["refreshToken"] = refresh_token;
            Http::post("https://api.bambulab.com/v1/user-service/user/refreshtoken")
                .header("Content-Type", "application/json")
                .header("Accept", "application/json")
                .set_post_body(ref_req.dump())
                .on_complete([&ref_body, &ref_status](std::string body, unsigned status) {
                    ref_body = std::move(body); ref_status = status; })
                .on_error([&ref_body, &ref_status](std::string body, std::string err, unsigned status) {
                    ref_body = body + " err=" + err; ref_status = status; })
                .perform_sync();
            BOOST_LOG_TRIVIAL(info) << "thirdparty_login: refreshtoken exchange status=" << ref_status
                                    << ", body len=" << ref_body.size();
            try {
                json rj = json::parse(ref_body);
                std::string fields;
                for (auto it = rj.begin(); it != rj.end(); ++it) {
                    if (!fields.empty()) fields += ", ";
                    fields += it.key() + "(" + (it.value().is_string()
                        ? std::to_string(it.value().get<std::string>().size()) + " chars"
                        : it.value().dump().substr(0, 24)) + ")";
                }
                BOOST_LOG_TRIVIAL(info) << "thirdparty_login: refreshtoken fields: " << fields;
                if (rj.contains("accessToken"))
                    log_token_shape("refreshed", rj["accessToken"].get<std::string>());
            } catch (...) {
                BOOST_LOG_TRIVIAL(info) << "thirdparty_login: refreshtoken body not json, head="
                                        << ref_body.substr(0, 160);
            }
        }
        try {
            json token_dbg = json::parse(token_body);
            std::string fields;
            for (auto it = token_dbg.begin(); it != token_dbg.end(); ++it) {
                if (!fields.empty())
                    fields += ", ";
                fields += it.key() + "(" + (it.value().is_string() ? std::to_string(it.value().get<std::string>().size()) + " chars"
                                                                   : it.value().dump().substr(0, 24)) + ")";
            }
            BOOST_LOG_TRIVIAL(info) << "thirdparty_login: token response fields: " << fields;
        } catch (...) {}

        unsigned int profile_http_code = 0;
        std::string  profile_body;
        int          profile_result = agent->get_my_profile(access_token, &profile_http_code, &profile_body, provider);
        if (profile_result != 0) {
            BOOST_LOG_TRIVIAL(warning) << "thirdparty_login: get_my_profile failed, http_code=" << profile_http_code;
            return fail_redirect("get_user_profile_error_" + std::to_string(profile_result));
        }

        std::string user_id;
        std::string user_name;
        std::string user_account;
        std::string user_avatar;
        try {
            json user_j = json::parse(profile_body);
            if (user_j.contains("uidStr"))
                user_id = user_j["uidStr"].get<std::string>();
            if (user_j.contains("name"))
                user_name = user_j["name"].get<std::string>();
            if (user_j.contains("avatar"))
                user_avatar = user_j["avatar"].get<std::string>();
            if (user_j.contains("account"))
                user_account = user_j["account"].get<std::string>();
        } catch (...) {
            BOOST_LOG_TRIVIAL(warning) << "thirdparty_login: profile JSON parse failed";
        }

        json j;
        j["data"]["refresh_token"]      = refresh_token;
        j["data"]["token"]              = access_token;
        j["data"]["expires_in"]         = expires_in_str;
        j["data"]["refresh_expires_in"] = refresh_expires_in_str;
        j["data"]["user"]["uid"]        = user_id;
        j["data"]["user"]["name"]       = user_name;
        j["data"]["user"]["account"]    = user_account;
        j["data"]["user"]["avatar"]     = user_avatar;
        const std::string login_json = j.dump();
        const int change_user_result = agent->change_user(login_json, provider);
        const bool logged_in = agent->is_user_login(provider);
        BOOST_LOG_TRIVIAL(info) << "thirdparty_login: profile http_code=" << profile_http_code
                                << ", user_id=" << user_id << ", change_user=" << change_user_result
                                << ", is_user_login=" << logged_in
                                << ", agent_user_id=" << agent->get_user_id(provider)
                                << ", login_json len=" << login_json.size();
        {
            // Straight after the handover, make one authenticated call so the log shows whether
            // the plug-in's own requests are accepted at this point.
            unsigned int probe_code = 0;
            std::string  probe_body;
            const int    probe_result = agent->get_user_print_info(&probe_code, &probe_body, provider);
            BOOST_LOG_TRIVIAL(info) << "thirdparty_login: post-login probe get_user_print_info result=" << probe_result
                                    << ", http_code=" << probe_code << ", body len=" << probe_body.size()
                                    << ", body head=" << probe_body.substr(0, 120);
        }
        if (logged_in) {
            wxGetApp().request_user_login(1, provider);
        }
        GUI::wxGetApp().CallAfter([] { wxGetApp().ShowUserLogin(false); });
        std::string location_str = (boost::format("%1%?result=success") % ticket_redirect_url).str();
        return std::make_shared<ResponseRedirect>(location_str);
    }

    return std::make_shared<ResponseNotFound>();
}

void HttpServer::ResponseNotFound::write_response(std::stringstream& ssOut)
{
    const std::string sHTML = "<html><body><h1>404 Not Found</h1><p>There's nothing here.</p></body></html>";
    ssOut << "HTTP/1.1 404 Not Found" << std::endl;
    ssOut << "content-type: text/html" << std::endl;
    ssOut << "content-length: " << sHTML.length() << std::endl;
    ssOut << std::endl;
    ssOut << sHTML;
}

void HttpServer::ResponseRedirect::write_response(std::stringstream& ssOut)
{
    const std::string sHTML =
        "<html><head><meta charset=\"utf-8\">"
        "<meta http-equiv=\"refresh\" content=\"0;url=" + location_str + "\">"
        "<style>body{font-family:Arial,sans-serif;background:#f7f7f7;color:#222;margin:32px;}"
        "a.button{display:inline-block;padding:10px 16px;margin-top:12px;background:#0f8bff;color:#fff;text-decoration:none;border-radius:6px;}"
        "</style></head><body><div class=\"container\">"
        "<h2>Authentication complete</h2>"
        "<p>You can return to OrcaSlicer. If your browser does not redirect automatically, use the button below.</p>"
        "<a class=\"button\" href=\"" + location_str + "\">Continue</a>"
        "<script>setTimeout(function(){try{window.close();}catch(e){}},1500);</script>"
        "</div></body></html>";
    ssOut << "HTTP/1.1 302 Found" << std::endl;
    ssOut << "Location: " << location_str << std::endl;
    ssOut << "content-type: text/html" << std::endl;
    ssOut << "content-length: " << sHTML.length() << std::endl;
    ssOut << std::endl;
    ssOut << sHTML;
}

void HttpServer::ResponseHtml::write_response(std::stringstream& ssOut)
{
    ssOut << "HTTP/1.1 200 OK" << std::endl;
    ssOut << "content-type: text/html" << std::endl;
    ssOut << "content-length: " << html.length() << std::endl;
    ssOut << std::endl;
    ssOut << html;
}

} // GUI
} //Slic3r

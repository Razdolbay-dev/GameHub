// ps5-homebrew-ui - GameHub API client implementation.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/api_client.hpp"

#include "third_party/parson/parson.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// ---- libSceHttp2 / libSceNet / libSceSsl declarations ----------------------
// These match samples/http2_get/main.c and the symbols exported by the
// target/lib/libSceHttp2.so stub. The SDK has no header for them.

extern "C"
{
    int sceNetInit();
    int sceNetPoolCreate(const char *name, int size, int flags);
    int sceNetPoolDestroy(int mem_id);

    int sceSslInit(std::size_t size);
    int sceSslTerm(int ctx_id);

    int sceHttp2Init(int libnet_mem_id, int libssl_ctx_id, std::size_t pool_size,
                     int http_version);
    int sceHttp2Term(int ctx_id);

    int sceHttp2CreateTemplate(int ctx_id, const char *user_agent, int http_version,
                               int is_auto_proxy_conf);
    int sceHttp2DeleteTemplate(int tmpl_id);

    int sceHttp2CreateRequestWithURL(int tmpl_id, const char *method, const char *url,
                                     std::uint64_t content_length);
    int sceHttp2DeleteRequest(int req_id);

    int sceHttp2AddRequestHeader(int req_id, const char *name, const char *value,
                                 int mode);
    int sceHttp2SendRequest(int req_id, const void *body, std::size_t size);
    int sceHttp2GetStatusCode(int req_id, int *status);
    int sceHttp2ReadData(int req_id, void *buffer, std::size_t size);
}

namespace hui::core
{

namespace
{

// Header modes for sceHttp2AddRequestHeader (Overwrite = 0, Add = 1).
constexpr int kHeaderModeOverwrite = 0;

// ---- parson helpers --------------------------------------------------------

std::string get_string(JSON_Object *obj, const char *key)
{
    if (obj == nullptr)
        return {};
    const char *value = json_object_get_string(obj, key);
    return value != nullptr ? std::string(value) : std::string();
}

int get_int(JSON_Object *obj, const char *key)
{
    if (obj == nullptr)
        return 0;
    return static_cast<int>(json_object_get_number(obj, key));
}

float get_float(JSON_Object *obj, const char *key)
{
    if (obj == nullptr)
        return 0.0f;
    JSON_Value *value = json_object_get_value(obj, key);
    if (value == nullptr)
        return 0.0f;
    if (json_value_get_type(value) == JSONString)
    {
        const char *text = json_value_get_string(value);
        return text != nullptr ? static_cast<float>(std::atof(text)) : 0.0f;
    }
    return static_cast<float>(json_value_get_number(value));
}

bool get_bool(JSON_Object *obj, const char *key)
{
    if (obj == nullptr)
        return false;
    JSON_Value *value = json_object_get_value(obj, key);
    if (value == nullptr)
        return false;
    if (json_value_get_type(value) == JSONBoolean)
        return json_value_get_boolean(value) != 0;
    if (json_value_get_type(value) == JSONNumber)
        return json_value_get_number(value) != 0.0;
    return false;
}

// ---- SceHttp2 session ------------------------------------------------------
//
// One session per request: init network -> create template -> create request
// -> add headers -> send -> read -> delete request -> delete template -> term.

class SceHttp2Session
{
  public:
    SceHttp2Session() = default;

    ~SceHttp2Session()
    {
        if (req_id_ >= 0)
            (void)sceHttp2DeleteRequest(req_id_);
        if (tmpl_id_ >= 0)
            (void)sceHttp2DeleteTemplate(tmpl_id_);
        if (http_id_ >= 0)
            (void)sceHttp2Term(http_id_);
        if (ssl_id_ >= 0)
            (void)sceSslTerm(ssl_id_);
        if (net_id_ >= 0)
            (void)sceNetPoolDestroy(net_id_);
    }

    SceHttp2Session(const SceHttp2Session &) = delete;
    SceHttp2Session &operator=(const SceHttp2Session &) = delete;

    bool open(const char *agent, const char *method, const std::string &url,
              std::uint64_t content_length, std::string &error)
    {
        if (sceNetInit() != 0)
        {
            error = "sceNetInit failed";
            return false;
        }
        net_id_ = sceNetPoolCreate("GameHub", 32 * 1024, 0);
        if (net_id_ < 0)
        {
            error = "sceNetPoolCreate failed";
            return false;
        }
        ssl_id_ = sceSslInit(256 * 1024);
        if (ssl_id_ < 0)
        {
            error = "sceSslInit failed";
            return false;
        }
        http_id_ = sceHttp2Init(net_id_, ssl_id_, 256 * 1024, 1);
        if (http_id_ < 0)
        {
            error = "sceHttp2Init failed";
            return false;
        }
        tmpl_id_ = sceHttp2CreateTemplate(http_id_, agent, 3, 1);
        if (tmpl_id_ < 0)
        {
            error = "sceHttp2CreateTemplate failed";
            return false;
        }
        req_id_ = sceHttp2CreateRequestWithURL(tmpl_id_, method, url.c_str(),
                                               content_length);
        if (req_id_ < 0)
        {
            error = "sceHttp2CreateRequestWithURL failed";
            return false;
        }
        return true;
    }

    bool add_header(const std::string &name, const std::string &value, int mode,
                    std::string &error)
    {
        if (sceHttp2AddRequestHeader(req_id_, name.c_str(), value.c_str(), mode) != 0)
        {
            error = "sceHttp2AddRequestHeader failed for " + name;
            return false;
        }
        return true;
    }

    bool send(const void *body, std::size_t size, std::string &error)
    {
        if (sceHttp2SendRequest(req_id_, body, size) != 0)
        {
            error = "sceHttp2SendRequest failed";
            return false;
        }
        return true;
    }

    int status_code()
    {
        int status = 0;
        if (sceHttp2GetStatusCode(req_id_, &status) != 0)
            return -1;
        return status;
    }

    // Read the whole body into `response` (used for JSON, which is text).
    bool read(std::string &response, int &status, std::string &error)
    {
        response.clear();
        status = status_code();
        if (status < 0)
        {
            error = "sceHttp2GetStatusCode failed";
            return false;
        }
        char buffer[4096];
        for (;;)
        {
            const int got = sceHttp2ReadData(req_id_, buffer, sizeof(buffer));
            if (got < 0)
            {
                error = "sceHttp2ReadData failed: " + std::to_string(got);
                return false;
            }
            if (got == 0)
                break;
            response.append(buffer, static_cast<std::size_t>(got));
        }
        return true;
    }

    int request_id() const noexcept
    {
        return req_id_;
    }

  private:
    int net_id_ = -1;
    int ssl_id_ = -1;
    int http_id_ = -1;
    int tmpl_id_ = -1;
    int req_id_ = -1;
};

} // namespace

ApiClient::~ApiClient() = default;

void ApiClient::global_init()
{
    // SceHttp2 is initialised per request in SceHttp2Session::open().
}

void ApiClient::global_cleanup()
{
    // Nothing global to clean up.
}

bool ApiClient::login(const std::string &email, const std::string &password,
                      std::string &error)
{
    error.clear();
    token_.clear();

    const std::string url = base_url_ + "/api/users/login";
    const std::string body =
        std::string("{\"email\":\"") + email +
        "\",\"password\":\"" + password + "\"}";

    SceHttp2Session session;
    if (!session.open("GameHub/1.0", "POST", url,
                      static_cast<std::uint64_t>(body.size()), error))
        return false;
    if (!session.add_header("Content-Type", "application/json",
                            kHeaderModeOverwrite, error))
        return false;
    if (!session.add_header("Accept", "application/json", kHeaderModeOverwrite,
                            error))
        return false;
    if (!session.send(body.data(), body.size(), error))
        return false;

    std::string response;
    int status = 0;
    if (!session.read(response, status, error))
        return false;
    if (status < 200 || status >= 300)
    {
        error = "HTTP " + std::to_string(status) + ": " + response;
        return false;
    }

    JSON_Value *root = json_parse_string(response.c_str());
    if (root == nullptr)
    {
        error = "JSON parse error";
        return false;
    }
    JSON_Object *obj = json_value_get_object(root);
    const std::string token = get_string(obj, "token");
    json_value_free(root);

    if (token.empty())
    {
        error = "login response has no token field";
        return false;
    }
    token_ = token;
    return true;
}

bool ApiClient::fetch_movies(int page, int per_page, std::vector<Movie> &out,
                             int &total_pages, std::string &error)
{
    out.clear();
    total_pages = 1;
    error.clear();

    if (token_.empty())
    {
        error = "not logged in: call login() first";
        return false;
    }

    const std::string url =
        base_url_ + "/api/content/movies?page=" + std::to_string(page) +
        "&per_page=" + std::to_string(per_page) + "&is_published=1";

    SceHttp2Session session;
    if (!session.open("GameHub/1.0", "GET", url, 0, error))
        return false;
    if (!session.add_header("Authorization", "Bearer " + token_,
                            kHeaderModeOverwrite, error))
        return false;
    if (!session.add_header("Accept", "application/json", kHeaderModeOverwrite,
                            error))
        return false;
    if (!session.send(nullptr, 0, error))
        return false;

    std::string response;
    int status = 0;
    if (!session.read(response, status, error))
        return false;
    if (status < 200 || status >= 300)
    {
        error = "HTTP " + std::to_string(status) + ": " + response;
        return false;
    }

    JSON_Value *root = json_parse_string(response.c_str());
    if (root == nullptr)
    {
        error = "JSON parse error";
        return false;
    }
    JSON_Object *obj = json_value_get_object(root);
    if (obj == nullptr)
    {
        json_value_free(root);
        error = "movies response is not a JSON object";
        return false;
    }

    JSON_Object *pagination = json_object_get_object(obj, "pagination");
    if (pagination != nullptr)
    {
        const int pages = static_cast<int>(json_object_get_number(pagination, "total_pages"));
        if (pages > 0)
            total_pages = pages;
    }

    JSON_Array *data = json_object_get_array(obj, "data");
    if (data == nullptr)
    {
        json_value_free(root);
        error = "movies response has no data array";
        return false;
    }

    const std::size_t count = json_array_get_count(data);
    out.reserve(count);
    for (std::size_t i = 0; i < count; ++i)
    {
        JSON_Object *entry = json_array_get_object(data, i);
        if (entry == nullptr)
            continue;
        Movie movie;
        movie.id = get_int(entry, "id");
        movie.title = get_string(entry, "title");
        movie.original_title = get_string(entry, "original_title");
        movie.release_date = get_string(entry, "release_date");
        movie.runtime = get_int(entry, "runtime");
        movie.vote_average = get_float(entry, "vote_average");
        movie.vote_count = get_int(entry, "vote_count");
        movie.poster_path = get_string(entry, "poster_path");
        movie.backdrop_path = get_string(entry, "backdrop_path");
        movie.overview = get_string(entry, "overview");
        movie.is_published = get_bool(entry, "is_published");
        out.push_back(std::move(movie));
    }
    json_value_free(root);
    return true;
}

bool ApiClient::fetch_bytes(const std::string &url,
                            std::vector<unsigned char> &out, std::string &error)
{
    out.clear();
    error.clear();

    SceHttp2Session session;
    if (!session.open("GameHub/1.0", "GET", url, 0, error))
        return false;
    if (!session.add_header("Accept", "image/*", kHeaderModeOverwrite, error))
        return false;
    if (!session.send(nullptr, 0, error))
        return false;

    const int status = session.status_code();
    if (status < 0)
    {
        error = "sceHttp2GetStatusCode failed";
        return false;
    }
    if (status < 200 || status >= 300)
    {
        error = "HTTP " + std::to_string(status);
        return false;
    }

    unsigned char buffer[4096];
    for (;;)
    {
        const int got = sceHttp2ReadData(session.request_id(), buffer, sizeof(buffer));
        if (got < 0)
        {
            error = "sceHttp2ReadData failed: " + std::to_string(got);
            return false;
        }
        if (got == 0)
            break;
        out.insert(out.end(), buffer, buffer + got);
    }
    return true;
}

} // namespace hui::core
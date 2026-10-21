// ps5-homebrew-ui - GameHub API client.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Minimal HTTP client for the GameHub API: login (JWT), movie list with
// pagination, and binary downloads (posters). Uses libSceHttp2 from the PS5
// payload SDK and the SDK-bundled parson parser for JSON.

#pragma once

#include <string>
#include <vector>

namespace hui::core
{

// A single movie as returned by /api/content/movies.
struct Movie
{
    int id = 0;
    std::string title;
    std::string original_title;
    std::string release_date;
    int runtime = 0;
    float vote_average = 0.0f;
    int vote_count = 0;
    std::string poster_path;
    std::string backdrop_path;
    std::string overview;
    bool is_published = false;
};

class ApiClient
{
  public:
    ApiClient() = default;
    ~ApiClient();

    ApiClient(const ApiClient &) = delete;
    ApiClient &operator=(const ApiClient &) = delete;

    // Global init/cleanup. SceHttp2 is initialised per request, so these are
    // no-ops kept for API compatibility with the previous libcurl version.
    static void global_init();
    static void global_cleanup();

    // POST /api/users/login. On success stores the JWT and returns true.
    bool login(const std::string &email, const std::string &password,
               std::string &error);

    // GET /api/content/movies?page=...&per_page=...&is_published=1
    // Requires a successful login() first. `total_pages` receives the
    // pagination.total_pages field from the response (1 if absent).
    bool fetch_movies(int page, int per_page, std::vector<Movie> &out,
                      int &total_pages, std::string &error);

    // GET a binary resource (e.g. a JPEG poster) into `out`.
    bool fetch_bytes(const std::string &url, std::vector<unsigned char> &out,
                     std::string &error);

    // The JWT from the last successful login, or an empty string.
    const std::string &token() const noexcept
    {
        return token_;
    }

    // The base URL, e.g. "http://inpulse.pit.su". No trailing slash.
    const std::string &base_url() const noexcept
    {
        return base_url_;
    }
    void set_base_url(std::string base)
    {
        base_url_ = std::move(base);
    }

  private:
    std::string base_url_ = "http://inpulse.pit.su";
    std::string token_;
};

} // namespace hui::core
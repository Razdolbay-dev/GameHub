// ps5-homebrew-ui - Application entry point.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Opens the display, controller and audio, loads the first page of the
// GameHub movie list and its posters, then runs the shell every frame. When
// the user navigates past the last card, paper.cpp calls request_more_movies()
// to append the next page.

#include "core/api_client.hpp"
#include "core/cover_loader.hpp"

#include "app/shell.hpp"
#include "audio/cues.hpp"
#include "audio/mixer.hpp"
#include "audio/music.hpp"
#include "core/frame_stats.hpp"
#include "core/input.hpp"
#include "core/save_file.hpp"
#include "core/settings.hpp"
#include "core/version.hpp"
#include "demo/catalog.hpp"
#include "gfx/gl_batch.hpp"
#include "gfx/renderer.hpp"
#include "platform/ps5/audio_out.hpp"
#include "platform/ps5/display_egl.hpp"
#include "platform/ps5/pad.hpp"
#include "platform/ps5/system.hpp"
#include "ui/fonts.hpp"

#include <GL/glcorearb.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

extern "C" void hui_heap_stats(std::size_t *live_bytes, std::size_t *peak_bytes,
                               std::size_t *blocks, std::size_t *failures);

namespace
{

constexpr const char *kAssets = "/app0/assets";
constexpr const char *kDataRoot = "/download0/hui";
constexpr int kPerPage = 30;

// Pagination state and the objects request_more_movies() needs. They live for
// the whole process, so paper.cpp can call the hook at any time.
hui::core::ApiClient *g_api = nullptr;
hui::demo::Catalog *g_catalog = nullptr;
hui::gfx::Renderer *g_renderer = nullptr;
int g_current_page = 0;
int g_total_pages = 1;
bool g_loading_more = false;

// demo::Item stores const char*. Keep every string alive for as long as the
// catalog is used. The vector is reserved up front and only grows by push_back
// between pages, never during a page, so pointers stay stable.
std::vector<std::string> g_owned_strings;

void log_heap(std::uint64_t frames)
{
    std::size_t live = 0;
    std::size_t peak = 0;
    std::size_t blocks = 0;
    std::size_t failures = 0;
    hui_heap_stats(&live, &peak, &blocks, &failures);
    hui::sys::log("[HUI] heap frames=%llu live=%zu peak=%zu blocks=%zu failures=%zu",
                  static_cast<unsigned long long>(frames), live, peak, blocks, failures);
}

bool open_display(hui::ps5::Display &display, int *resolution)
{
    using hui::Settings;
    const Settings::Resolution &mode = Settings::kResolutions[*resolution];
    if (display.open(mode.width, mode.height))
    {
        hui::sys::log("[HUI] display mode %s %dx%d", mode.label, display.width(),
                      display.height());
        return true;
    }
    if (*resolution == 0)
        return false;
    hui::sys::log("[HUI] display mode %s failed, using 1080p", mode.label);
    *resolution = 0;
    return display.open(Settings::kResolutions[0].width, Settings::kResolutions[0].height);
}

bool load_font(hui::gfx::Renderer &renderer, const char *name, hui::gfx::Font *font,
               hui::ui::FontRef *ref)
{
    std::string data;
    const std::string path = std::string(kAssets) + "/fonts/" + name;
    if (!hui::save::read_file(path, &data) || !font->load(data))
    {
        hui::sys::log("[HUI] font %s failed: %s", name, font->error().c_str());
        return false;
    }
    ref->font = font;
    ref->texture = renderer.batch().create_font_texture(*font);
    return true;
}

// Convert a page of API movies into demo::Item, downloading each poster and
// appending the items to `out`.
void movies_to_items(hui::core::ApiClient &api, hui::gfx::Renderer &renderer,
                     const std::vector<hui::core::Movie> &movies,
                     std::vector<hui::demo::Item> &out)
{
    g_owned_strings.reserve(g_owned_strings.size() + movies.size() * 4);

    for (const auto &m : movies)
    {
        g_owned_strings.push_back(m.title.empty() ? "Untitled" : m.title);
        g_owned_strings.push_back(m.original_title.empty() ? "Unknown" : m.original_title);
        g_owned_strings.push_back("Movie");
        g_owned_strings.push_back(m.overview.empty() ? "No description." : m.overview);

        const std::size_t n = g_owned_strings.size();
        hui::demo::Item item{};
        item.title = g_owned_strings[n - 4].c_str();
        item.studio = g_owned_strings[n - 3].c_str();
        item.genre = g_owned_strings[n - 2].c_str();
        item.blurb = g_owned_strings[n - 1].c_str();
        item.year = 0;
        // vote_average is 0..10 in the API; Item::rating is 0..5.
        item.rating = m.vote_average > 5.0f ? m.vote_average / 2.0f : m.vote_average;
        item.progress = 0.0f;
        item.hours = m.runtime / 60;
        item.players = 1;
        item.dark = hui::gfx::Color::rgb(0x101020);
        item.mid = hui::gfx::Color::rgb(0x404080);
        item.accent = hui::gfx::Color::rgb(0xffd166);

        if (!m.poster_path.empty())
        {
            const std::string cover_url =
                api.base_url() + "/api/image/w500" + m.poster_path;
            std::string cover_error;
            const std::uint32_t cover =
                hui::core::load_cover(api, renderer.batch(), cover_url, cover_error);
            if (cover == 0)
                hui::sys::log("[HUI] cover failed %s: %s", cover_url.c_str(),
                              cover_error.c_str());
            else
                item.cover = cover;
        }
        out.push_back(item);
    }
}

// Load one page (1-based) and append it to the catalog. Returns false on a
// network/parse error; the catalog is untouched in that case.
bool load_page(int page)
{
    if (g_api == nullptr || g_catalog == nullptr || g_renderer == nullptr)
        return false;

    std::vector<hui::core::Movie> movies;
    std::string error;
    int total_pages = g_total_pages;
    if (!g_api->fetch_movies(page, kPerPage, movies, total_pages, error))
    {
        hui::sys::log("[HUI] fetch page %d failed: %s", page, error.c_str());
        return false;
    }
    hui::sys::log("[HUI] page %d/%d movies=%zu", page, total_pages, movies.size());
    g_total_pages = total_pages;
    g_current_page = page;

    std::vector<hui::demo::Item> items;
    items.reserve(movies.size());
    movies_to_items(*g_api, *g_renderer, movies, items);
    if (page == 1)
        g_catalog->set_items(std::move(items));    // заменить демо-каталог
    else
        g_catalog->append_items(std::move(items)); // добавить к своим
    return true;
}

} // namespace

// Called from paper.cpp when the focus reaches the end of the visible list.
// Appends the next page to the catalog; paper.cpp notices the growth and
// re-flows the grid. Must be outside the anonymous namespace so the linker
// can find it.
void request_more_movies()
{
    if (g_loading_more || g_current_page >= g_total_pages)
        return;
    g_loading_more = true;
    (void)load_page(g_current_page + 1);
    g_loading_more = false;
}

int main()
{
    using namespace hui;
    sys::log("[HUI] entry");
    sys::log("[HUI] storage dir=%d", save::ensure_directory(kDataRoot) ? 1 : 0);

    const Settings saved = app::Shell::load_settings(kDataRoot);
    int resolution = ps5::Display::supports_display_modes() ? saved.resolution : 0;
    ps5::Display display;
    if (!open_display(display, &resolution))
    {
        sys::log("[HUI] fatal: display open failed");
        sys::park();
    }

    gfx::Renderer renderer;
    gfx::Font regular;
    gfx::Font semibold;
    gfx::Font display_font;
    gfx::Font mono;
    gfx::Font pixel;
    gfx::Font hand;
    ui::Fonts fonts;
    if (!renderer.init() ||
        !load_font(renderer, "inter-regular.huifont", &regular, &fonts.regular) ||
        !load_font(renderer, "inter-semibold.huifont", &semibold, &fonts.semibold) ||
        !load_font(renderer, "montserrat-medium.huifont", &display_font, &fonts.display) ||
        !load_font(renderer, "dejavu-sans-mono.huifont", &mono, &fonts.mono) ||
        !load_font(renderer, "press-start-2p.huifont", &pixel, &fonts.pixel) ||
        !load_font(renderer, "patrick-hand.huifont", &hand, &fonts.hand))
    {
        sys::log("[HUI] fatal: renderer init failed");
        sys::park();
    }

    demo::Catalog catalog;

    // The API client lives for the whole process so request_more_movies() can
    // reuse the JWT and the base URL after the first page.
    static core::ApiClient api;
    core::ApiClient::global_init();
    g_api = &api;
    g_catalog = &catalog;
    g_renderer = &renderer;

    std::string error;
    if (!api.login("chulkov@netonline.ru", "secure123", error))
        sys::log("[HUI] api login failed: %s", error.c_str());
    else
        (void)load_page(1);

    const std::int64_t art_start = sys::monotonic_us();
    if (!catalog.build_covers(renderer, fonts))
        sys::log("[HUI] cover art failed; designs draw without it");
    sys::log("[HUI] cover art %zu covers in %lld ms", catalog.size(),
             static_cast<long long>((sys::monotonic_us() - art_start) / 1000));

    ps5::Pad pad;
    pad.open();
    InputTracker tracker;
    audio::Mixer mixer;
    audio::MusicPlayer music;
    const int tracks = music.init(mixer, std::string(kAssets) + "/audio/music",
                                  static_cast<std::uint64_t>(sys::monotonic_us()));
    sys::log("[HUI] music songs=%d", tracks);
    ps5::AudioOut audio_out;
    audio_out.start(mixer);
    audio::SoundBank sounds;
    const auto bank = sounds.load(std::string(kAssets) + "/audio/sfx");
    sys::log("[HUI] sounds files=%d rejected=%d", bank.files, bank.rejected);
    for (const std::string &sound_error : bank.errors)
        sys::log("[HUI] sound rejected %s", sound_error.c_str());

    app::Shell shell(fonts, catalog, kDataRoot, renderer.glass_texture());
    const std::string version = read_content_version("/app0/sce_sys/param.json");
    shell.set_version(version);
    sys::log("[HUI] version %s designs=%zu", version.empty() ? "unknown" : version.c_str(),
             shell.concept_count());

    std::int64_t previous = sys::monotonic_us();
    std::uint64_t frames = 0;
    FrameStats stats;
    double fps_seconds = 0.0;
    int fps_frames = 0;
    PadSample samples[64];
    std::int64_t last_frame_start = sys::monotonic_us();
    for (;;)
    {
        const std::int64_t now = sys::monotonic_us();
        float dt = frames == 0 ? 1.0f / 60.0f
                               : static_cast<float>(now - last_frame_start) / 1e6f;
        last_frame_start = now;
        if (dt > 0.05f)
            dt = 0.05f;
        const std::size_t count = pad.read(samples);
        InputFrame input = tracker.update(std::span<const PadSample>(samples, count),
                                          static_cast<std::uint64_t>(now));

        shell.update(input, dt);
        if (shell.take_settings_changed())
        {
            const Settings &settings = shell.settings();
            mixer.set_bus_gain(audio::Bus::music, Settings::gain(settings.music_volume));
            mixer.set_bus_gain(audio::Bus::sfx, Settings::gain(settings.sfx_volume));
            mixer.set_bus_gain(audio::Bus::ui, Settings::gain(settings.ui_volume));
            InputSettings input_settings = tracker.settings();
            input_settings.swap_confirm = settings.swap_confirm;
            tracker.set_settings(input_settings);
        }
        const app::Feedback &feedback = shell.feedback();
        for (const audio::CueEvent &event : feedback.cues)
        {
            sounds.play(mixer, event.set == audio::SoundSet::count ? shell.sound_set() : event.set,
                        event);
            if (event.cue == audio::Cue::complete || event.cue == audio::Cue::welcome)
                music.duck();
        }
        if (shell.settings().haptics && feedback.rumble_strength > 0.0f)
            pad.rumble(feedback.rumble_strength, feedback.rumble_seconds);
        pad.tick(dt);
        if (shell.settings().light_bar)
        {
            const gfx::Color accent = shell.accent();
            pad.set_light_bar(static_cast<std::uint8_t>(accent.r * 255.0f),
                              static_cast<std::uint8_t>(accent.g * 255.0f),
                              static_cast<std::uint8_t>(accent.b * 255.0f));
        }
        music.pump(dt);

        shell.compose(renderer);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
        renderer.present(0, display.width(), display.height());
        if (!display.swap())
        {
            sys::log("[HUI] fatal: swap failed frame=%llu error=%s",
                     static_cast<unsigned long long>(frames),
                     ps5::egl_error_name(display.last_error()));
            sys::park();
        }
        ++frames;
        const std::int64_t presented = sys::monotonic_us();
        const float frame_ms = static_cast<float>(presented - previous) / 1000.0f;
        app::Telemetry &telemetry = shell.telemetry();
        if (frames == 1)
        {
            sys::log("[HUI] first-swap ok shapes=%zu draws=%zu", renderer.last_instances(),
                     renderer.last_draw_calls());
            const bool hidden = sys::hide_splash_screen();
            sys::log("[HUI] ready splash_hidden=%d", hidden ? 1 : 0);
            log_heap(frames);
        }
        else
        {
            stats.add(static_cast<double>(frame_ms));
            telemetry.push(frame_ms);
        }
        previous = presented;
        telemetry.draw_calls = renderer.last_draw_calls();
        telemetry.instances = renderer.last_instances();
        telemetry.voices = mixer.active_voices();
        fps_seconds += static_cast<double>(frame_ms) / 1000.0;
        ++fps_frames;
        if (fps_seconds >= 0.5)
        {
            telemetry.fps = static_cast<float>(fps_frames / fps_seconds);
            telemetry.average_ms = static_cast<float>(fps_seconds * 1000.0 / fps_frames);
            fps_seconds = 0.0;
            fps_frames = 0;
        }
        if (stats.count() == 600)
        {
            char summary[160];
            stats.format(summary, sizeof(summary));
            sys::log("[HUI] %s draws=%zu shapes=%zu", summary, renderer.last_draw_calls(),
                     renderer.last_instances());
            sys::log("[HUI] audio grains=%llu errors=%llu voices=%d music=%s underruns=%llu",
                     static_cast<unsigned long long>(audio_out.grains()),
                     static_cast<unsigned long long>(audio_out.errors()), mixer.active_voices(),
                     music.current().c_str(),
                     static_cast<unsigned long long>(mixer.stream_underruns()));
            stats.reset();
            if (frames % 3600 < 600)
                log_heap(frames);
        }
    }
}
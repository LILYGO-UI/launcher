#include "system_controls.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <thread>
#include <utility>
#include <fcntl.h>
#include <unistd.h>

#if defined(__linux__) && !defined(LAUNCHER_SIMULATOR)
#include <alsa/asoundlib.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
extern char **environ;
#endif

namespace launcher {
namespace {

namespace fs = std::filesystem;

bool read_nonnegative(const fs::path &path, long &value)
{
    std::ifstream input(path);
    if (!(input >> value) || value < 0) return false;
    input >> std::ws;
    return input.eof();
}

int percentage(long value, long maximum)
{
    if (maximum <= 0) return 0;
    const double fraction = static_cast<double>(value) / maximum;
    return static_cast<int>(std::lround(std::clamp(fraction, 0.0, 1.0) * 100.0));
}

#if defined(__linux__) && !defined(LAUNCHER_SIMULATOR)

struct Mixer {
    snd_mixer_t *handle = nullptr;

    explicit Mixer(const char *card)
    {
        if (snd_mixer_open(&handle, 0) < 0) return;
        if (snd_mixer_attach(handle, card) < 0 ||
            snd_mixer_selem_register(handle, nullptr, nullptr) < 0 ||
            snd_mixer_load(handle) < 0) {
            snd_mixer_close(handle);
            handle = nullptr;
        }
    }
    ~Mixer() { if (handle) snd_mixer_close(handle); }
    Mixer(const Mixer &) = delete;
    Mixer &operator=(const Mixer &) = delete;
};

snd_mixer_elem_t *find_element(snd_mixer_t *mixer, const std::string &name)
{
    for (auto *element = snd_mixer_first_elem(mixer); element;
         element = snd_mixer_elem_next(element)) {
        if (snd_mixer_selem_is_active(element) &&
            snd_mixer_selem_get_index(element) == 0 &&
            snd_mixer_selem_has_playback_volume(element) &&
            name == snd_mixer_selem_get_name(element)) return element;
    }
    return nullptr;
}

// Use the playback endpoint rather than capture gain or unrelated mixer paths.
// ES8389 exposes left/right DACs as two independent simple mixer elements.
std::vector<std::string> playback_elements(snd_mixer_t *mixer)
{
    // Standard ALSA mixer names omit the Playback suffix, while codec ASoC
    // controls retain it. Try both forms so desktop cards work with either
    // USB/HDMI mixers or the CM0's ES8389/ES8311 controls.
    for (const char *name : {"Master", "Master Playback Volume", "Speaker",
                             "Speaker Playback Volume", "Headphone",
                             "Headphone Playback Volume", "PCM", "PCM Playback Volume",
                             "DAC", "DAC Playback Volume"}) {
        if (find_element(mixer, name)) return {name};
    }
    /* The CM0 ES8389 driver exposes its DAC channels as the short simple
     * mixer names DACL/DACR.  Some ALSA revisions append "Playback Volume";
     * accept both forms so the target board and desktop codecs share the
     * same path. */
    for (const auto &pair : {std::pair<const char *, const char *>{"DACL", "DACR"},
                             std::pair<const char *, const char *>{"DACL Playback Volume", "DACR Playback Volume"}}) {
        if (find_element(mixer, pair.first) && find_element(mixer, pair.second))
            return {pair.first, pair.second};
    }
    return {};
}

struct VolumeRange {
    long minimum = 0;
    long maximum = 0;
    bool decibels = false;
};

VolumeRange volume_range(snd_mixer_elem_t *element)
{
    VolumeRange range;
    if (snd_mixer_selem_get_playback_dB_range(element, &range.minimum,
                                             &range.maximum) >= 0 &&
        range.minimum < range.maximum) {
        // Hardware gain above unity can clip; 100% is at most 0 dB.
        range.maximum = std::min(0L, range.maximum);
        if (range.minimum < range.maximum) {
            range.decibels = true;
            return range;
        }
    }
    snd_mixer_selem_get_playback_volume_range(element, &range.minimum,
                                             &range.maximum);
    return range;
}

double volume_fraction(long value, const VolumeRange &range)
{
    if (range.minimum >= range.maximum) return 0;
    if (!range.decibels)
        return static_cast<double>(value - range.minimum) /
               static_cast<double>(range.maximum - range.minimum);
    // Cubic amplitude mapping, as used by ALSA's perceptual volume sliders.
    const double minimum = std::pow(10.0, (range.minimum - range.maximum) / 6000.0);
    return (std::pow(10.0, (value - range.maximum) / 6000.0) - minimum) /
           (1.0 - minimum);
}

bool read_volume(snd_mixer_t *mixer, const std::vector<std::string> &names,
                 int &percent)
{
    double total = 0;
    unsigned int channels = 0;
    for (const auto &name : names) {
        auto *element = find_element(mixer, name);
        if (!element) return false;
        const auto range = volume_range(element);
        if (range.maximum <= range.minimum) return false;
        for (int channel = 0; channel <= SND_MIXER_SCHN_LAST; ++channel) {
            const auto id = static_cast<snd_mixer_selem_channel_id_t>(channel);
            if (!snd_mixer_selem_has_playback_channel(element, id)) continue;
            long value = 0;
            const int result = range.decibels
                ? snd_mixer_selem_get_playback_dB(element, id, &value)
                : snd_mixer_selem_get_playback_volume(element, id, &value);
            if (result < 0) return false;
            int enabled = 1;
            if (snd_mixer_selem_has_playback_switch(element) &&
                snd_mixer_selem_get_playback_switch(element, id, &enabled) < 0)
                return false;
            total += enabled ? std::clamp(volume_fraction(value, range), 0.0, 1.0) : 0.0;
            ++channels;
        }
    }
    if (!channels) return false;
    percent = static_cast<int>(std::lround(total * 100 / channels));
    return true;
}

bool write_volume(snd_mixer_t *mixer, const std::vector<std::string> &names,
                  int percent)
{
    for (const auto &name : names) {
        auto *element = find_element(mixer, name);
        if (!element) return false;
        const auto range = volume_range(element);
        if (range.maximum <= range.minimum) return false;
        long value = range.minimum;
        int result = 0;
        if (range.decibels) {
            if (percent > 0) {
                const double minimum = std::pow(10.0, (range.minimum - range.maximum) / 6000.0);
                const double amplitude = minimum + (1.0 - minimum) * percent / 100.0;
                value = range.maximum + static_cast<long>(std::lround(6000.0 * std::log10(amplitude)));
            }
            result = snd_mixer_selem_set_playback_dB_all(element, value, -1);
        } else {
            value += static_cast<long>(std::lround(
                static_cast<double>(range.maximum - range.minimum) * percent / 100.0));
            result = snd_mixer_selem_set_playback_volume_all(element, value);
        }
        if (result < 0) return false;
        if (snd_mixer_selem_has_playback_switch(element) &&
            snd_mixer_selem_set_playback_switch_all(element, percent > 0) < 0)
            return false;
    }
    return true;
}

const char *systemctl_path()
{
    for (const char *path : {"/usr/bin/systemctl", "/bin/systemctl"})
        if (access(path, X_OK) == 0) return path;
    return nullptr;
}

#endif

} // namespace

SystemControls::SystemControls(bool simulated, std::string backlight_directory)
    : backlight_directory_(std::move(backlight_directory))
{
#if defined(LAUNCHER_SIMULATOR)
    (void)simulated;
    state_.simulated = true;
#else
    state_.simulated = simulated;
#endif
    if (state_.simulated) {
        state_.brightness_available = true;
        state_.brightness_percent = 65;
        state_.volume_available = true;
        state_.volume_percent = 45;
        state_.power_off_available = true;
    }
    refresh();
}

const SystemControlsState &SystemControls::refresh()
{
    if (state_.simulated) return state_;
    refresh_brightness();
    refresh_volume();
#if defined(__linux__) && !defined(LAUNCHER_SIMULATOR)
    state_.power_off_available = systemctl_path() && access("/run/systemd/system", F_OK) == 0;
#endif
    return state_;
}

void SystemControls::refresh_brightness()
{
    state_.brightness_available = false;
    state_.brightness_software = false;
    brightness_path_.clear();
    brightness_maximum_ = 0;
    std::error_code error;
    std::vector<fs::path> devices;
    fs::directory_iterator iterator(backlight_directory_, error);
    const fs::directory_iterator end;
    while (!error && iterator != end) {
        devices.push_back(iterator->path());
        iterator.increment(error);
    }
    std::sort(devices.begin(), devices.end());
    for (const auto &device : devices) {
        long maximum = 0;
        long current = 0;
        const auto brightness = device / "brightness";
        if (!read_nonnegative(device / "max_brightness", maximum) || maximum <= 0 ||
            maximum > std::numeric_limits<int>::max() ||
            !read_nonnegative(brightness, current) || current > maximum ||
            access(brightness.c_str(), W_OK) != 0) continue;
        brightness_path_ = brightness;
        brightness_maximum_ = maximum;
        state_.brightness_percent = percentage(current, maximum);
        state_.brightness_available = true;
        return;
    }

    /* CM0's RM69A10 AMOLED is driven over DSI and its panel driver does not
     * register /sys/class/backlight. Keep the control usable with the
     * launcher scene dimmer. Custom test directories still report hardware
     * as unavailable so failures remain observable in tests. */
    if (backlight_directory_ == "/sys/class/backlight") {
        state_.brightness_available = true;
        state_.brightness_software = true;
        state_.brightness_percent = std::clamp(state_.brightness_percent, 1, 100);
    }
}

void SystemControls::refresh_volume()
{
    state_.volume_available = false;
    volume_card_.clear();
    volume_elements_.clear();
#if defined(__linux__) && !defined(LAUNCHER_SIMULATOR)
    struct Card { int number; int preference; };
    std::vector<Card> cards;
    int card = -1;
    while (snd_card_next(&card) >= 0 && card >= 0) {
        char *raw_name = nullptr;
        snd_card_get_name(card, &raw_name);
        const std::string name = raw_name ? raw_name : "";
        std::free(raw_name);
        // The built-in CM0 codec takes precedence over HDMI/USB sound cards.
        const int preference = name.find("ES8389") != std::string::npos ? 0 :
                               name.find("ES8311") != std::string::npos ? 1 : 2;
        cards.push_back({card, preference});
    }
    std::stable_sort(cards.begin(), cards.end(), [](const Card &a, const Card &b) {
        return a.preference < b.preference;
    });
    for (const auto &candidate : cards) {
        const std::string device = "hw:" + std::to_string(candidate.number);
        Mixer mixer(device.c_str());
        if (!mixer.handle) continue;
        const auto names = playback_elements(mixer.handle);
        int percent = 0;
        if (names.empty() || !read_volume(mixer.handle, names, percent)) continue;
        volume_card_ = device;
        volume_elements_ = names;
        state_.volume_percent = percent;
        state_.volume_available = true;
        return;
    }
#endif
}

bool SystemControls::fail(const char *message)
{
    error_ = message;
    return false;
}

bool SystemControls::set_brightness(int percent)
{
    error_.clear();
    percent = std::clamp(percent, 1, 100);
    if (state_.simulated) {
        state_.brightness_percent = percent;
        return true;
    }
    refresh_brightness();
    if (!state_.brightness_available) return fail("Screen brightness is unavailable");
    if (state_.brightness_software) {
        state_.brightness_percent = percent;
        return true;
    }
    const long value = std::max(1L, static_cast<long>(std::lround(
        static_cast<double>(brightness_maximum_) * percent / 100.0)));
    const int descriptor = open(brightness_path_.c_str(), O_WRONLY | O_CLOEXEC | O_TRUNC);
    if (descriptor < 0) return fail("Could not change screen brightness");
    const std::string text = std::to_string(value) + "\n";
    ssize_t written;
    do { written = write(descriptor, text.data(), text.size()); }
    while (written < 0 && errno == EINTR);
    const bool closed = close(descriptor) == 0;
    refresh_brightness();
    if (written != static_cast<ssize_t>(text.size()) || !closed ||
        !state_.brightness_available) return fail("Could not change screen brightness");
    return true;
}

bool SystemControls::set_volume(int percent)
{
    error_.clear();
    percent = std::clamp(percent, 0, 100);
    if (state_.simulated) {
        state_.volume_percent = percent;
        return true;
    }
    refresh_volume();
    if (!state_.volume_available) return fail("Audio volume is unavailable");
#if defined(__linux__) && !defined(LAUNCHER_SIMULATOR)
    Mixer mixer(volume_card_.c_str());
    const bool written = mixer.handle && write_volume(mixer.handle, volume_elements_, percent);
    refresh_volume();
    if (!written || !state_.volume_available) return fail("Could not change audio volume");
    return true;
#else
    return fail("Audio volume is unavailable");
#endif
}

bool SystemControls::power_off()
{
    error_.clear();
    if (state_.simulated) return true;
#if defined(__linux__) && !defined(LAUNCHER_SIMULATOR)
    const char *command = systemctl_path();
    if (!command || !state_.power_off_available) return fail("Power off is unavailable");
    char *arguments[] = {const_cast<char *>(command), const_cast<char *>("--no-block"),
                         const_cast<char *>("--no-ask-password"),
                         const_cast<char *>("poweroff"), nullptr};
    pid_t child = -1;
    if (posix_spawn(&child, command, nullptr, nullptr, arguments, environ) != 0)
        return fail("Could not request power off");
    // systemctl only queues the orderly shutdown; bound IPC waits on a broken bus.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    int status = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        const pid_t result = waitpid(child, &status, WNOHANG);
        if (result == child) {
            if (WIFEXITED(status) && WEXITSTATUS(status) == 0) return true;
            return fail("The system could not power off");
        }
        if (result < 0 && errno != EINTR) return fail("Could not confirm power off");
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    kill(child, SIGKILL);
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    return fail("The power off request timed out");
#else
    return fail("Power off is unavailable");
#endif
}

} // namespace launcher

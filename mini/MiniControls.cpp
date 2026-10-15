// Copyright (c) 2026 Martial Systems LLC. All rights reserved.
#include "MiniControls.h"

#if JUCE_LINUX
#include <fcntl.h>
#include <linux/gpio.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <cstring>
#endif

namespace mini {

static const char* kNames[] = {"SYNTH", "DRUM", "TAPE", "MIX", "OCT-", "OCT+", "LIFT", "DROP", "REC", "PLAY", "SHIFT"};

const char* btnName(Btn b) noexcept { return kNames[juce::jlimit(0, int(Btn::Count) - 1, int(b))]; }

Btn btnFromName(const juce::String& s) noexcept {
    for (int i = 0; i < int(Btn::Count); ++i)
        if (s.equalsIgnoreCase(kNames[i])) return Btn(i);
    return Btn::Count;
}

static void post(const Sink& sink, Event e) {
    if (juce::MessageManager::getInstance()->isThisTheMessageThread()) sink(e);
    else juce::MessageManager::callAsync([sink, e] { sink(e); });
}

bool keyboardEvent(const juce::KeyPress& k, bool down, const Sink& sink) {
    // keys: lower octave Z S X D C V G B H N J M, upper Q 2 W 3 E R 5 T 6 Y 7 U
    static const juce::String lower = "zsxdcvgbhnjm", upper = "q2w3er5t6y7u";
    const auto c = juce::CharacterFunctions::toLowerCase(static_cast<juce::juce_wchar>(k.getKeyCode()));
    if (int i = lower.indexOfChar(c); i >= 0) { sink({Event::Key, i, down}); return true; }
    if (int i = upper.indexOfChar(c); i >= 0) { sink({Event::Key, 12 + i, down}); return true; }
    struct Map { int code; Btn b; };
    static const Map map[] = {
        {juce::KeyPress::F1Key, Btn::Synth}, {juce::KeyPress::F2Key, Btn::Drum}, {juce::KeyPress::F3Key, Btn::Tape},
        {juce::KeyPress::F4Key, Btn::Mix},   {'-', Btn::OctDown}, {'=', Btn::OctUp}, {'l', Btn::Lift}, {'p', Btn::Drop},
        {'o', Btn::Rec}, {juce::KeyPress::spaceKey, Btn::PlayStop}, {juce::KeyPress::tabKey, Btn::Shift}};
    for (auto& m : map)
        if (c == m.code || k.getKeyCode() == m.code) { sink({Event::Button, int(m.b), down}); return true; }
    // encoders: ",." yellow, "[]" blue, "90" black, ";'" red (down/up)
    if (down) {
        static const char* turns[4] = {",.", "[]", "90", ";'"};
        for (int e = 0; e < 4; ++e) {
            if (c == turns[e][0]) { sink({Event::Turn, e, -1}); return true; }
            if (c == turns[e][1]) { sink({Event::Turn, e, +1}); return true; }
        }
    }
    return false;
}

bool midiPanelEvent(const juce::MidiMessage& m, const Sink& sink) {
    if (m.getChannel() == 16) {
        if (m.isNoteOnOrOff() && m.getNoteNumber() < int(Btn::Count)) {
            post(sink, {Event::Button, m.getNoteNumber(), m.isNoteOn() ? 1 : 0});
            return true;
        }
        if (m.isController()) {
            const int cc = m.getControllerNumber(), v = m.getControllerValue();
            if (cc >= 20 && cc <= 23) { post(sink, {Event::Turn, cc - 20, v - 64}); return true; }
            if (cc >= 24 && cc <= 27) { post(sink, {Event::Push, cc - 24, v >= 64 ? 1 : 0}); return true; }
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
Gpio::Gpio(Sink sink) : sink_(std::move(sink)) {}
Gpio::~Gpio() { stop(); }

juce::String Gpio::start(const juce::File& json) {
#if JUCE_LINUX
    auto v = juce::JSON::parse(json);
    if (!v.isObject()) return "no GPIO config (" + json.getFullPathName() + ")";
    const auto chip = v.getProperty("chip", "/dev/gpiochip0").toString();
    const bool activeLow = v.getProperty("active_low", true);
    lines_.clear();
    const auto buttons = v.getProperty("buttons", {}), keys = v.getProperty("keys", {}), encs = v.getProperty("encoders", {});
    if (auto* b = buttons.getDynamicObject())
        for (const auto& p : b->getProperties()) {
            const auto id = btnFromName(p.name.toString());
            if (id != Btn::Count) lines_.push_back({int(p.value), 0, int(id), activeLow});
        }
    if (auto* k = keys.getArray())
        for (int i = 0; i < juce::jmin(24, k->size()); ++i) lines_.push_back({int((*k)[i]), 1, i, activeLow});
    if (auto* e = encs.getArray())
        for (int i = 0; i < juce::jmin(4, e->size()); ++i) {
            const auto o = (*e)[i];
            lines_.push_back({int(o.getProperty("a", -1)), 2, i, activeLow});
            lines_.push_back({int(o.getProperty("b", -1)), 3, i, activeLow});
            lines_.push_back({int(o.getProperty("push", -1)), 4, i, activeLow});
        }
    lines_.erase(std::remove_if(lines_.begin(), lines_.end(), [](auto& l) { return l.offset < 0; }), lines_.end());
    if (lines_.empty()) return "GPIO config lists no lines";
    if (lines_.size() > GPIO_V2_LINES_MAX) return "too many GPIO lines (max 64 per request)";

    const int chipFd = ::open(chip.toRawUTF8(), O_RDONLY | O_CLOEXEC);
    if (chipFd < 0) return "cannot open " + chip + " (is the user in the gpio group?)";
    gpio_v2_line_request req{};
    for (size_t i = 0; i < lines_.size(); ++i) req.offsets[i] = static_cast<__u32>(lines_[i].offset);
    req.num_lines = static_cast<__u32>(lines_.size());
    req.config.flags = GPIO_V2_LINE_FLAG_INPUT | (activeLow ? GPIO_V2_LINE_FLAG_BIAS_PULL_UP : 0);
    std::strncpy(req.consumer, "lift-mini", sizeof(req.consumer) - 1);
    const int r = ::ioctl(chipFd, GPIO_V2_GET_LINE_IOCTL, &req);
    ::close(chipFd);
    if (r < 0) return "GPIO line request failed (lines busy or wrong offsets)";
    fd_ = req.fd;
    run_ = true;
    thread_ = std::thread([this] { loop(); });
    return {};
#else
    juce::ignoreUnused(json);
    return "GPIO is Linux only";
#endif
}

void Gpio::stop() {
    run_ = false;
    if (thread_.joinable()) thread_.join();
#if JUCE_LINUX
    if (fd_ >= 0) ::close(fd_);
#endif
    fd_ = -1;
}

void Gpio::loop() {
#if JUCE_LINUX
    const size_t n = lines_.size();
    std::vector<int> stable(n, 0), count(n, 0);
    int encState[4] = {}, encAcc[4] = {};
    // quadrature table: (prev << 2 | now) -> step
    static const int quad[16] = {0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0};
    while (run_) {
        gpio_v2_line_values vals{};
        vals.mask = (n >= 64) ? ~0ull : ((1ull << n) - 1);
        if (::ioctl(fd_, GPIO_V2_LINE_GET_VALUES_IOCTL, &vals) == 0) {
            int encNow[4] = {encState[0] & 3, encState[1] & 3, encState[2] & 3, encState[3] & 3};
            bool encSeen[4] = {};
            for (size_t i = 0; i < n; ++i) {
                auto& l = lines_[i];
                int v = int((vals.bits >> i) & 1u);
                if (l.activeLow) v = 1 - v;
                if (l.kind == 2 || l.kind == 3) {  // encoder phases: no debounce, table decode
                    const int bit = l.kind == 2 ? 2 : 1;
                    encNow[l.index] = v ? (encNow[l.index] | bit) : (encNow[l.index] & ~bit);
                    encSeen[l.index] = true;
                    continue;
                }
                if (v != stable[i]) {
                    if (++count[i] >= 5) {  // 5 ms debounce
                        stable[i] = v; count[i] = 0;
                        const auto kind = l.kind == 0 ? Event::Button : l.kind == 1 ? Event::Key : Event::Push;
                        post(sink_, {kind, l.index, v});
                    }
                } else count[i] = 0;
            }
            for (int e = 0; e < 4; ++e) {
                if (!encSeen[e]) continue;
                const int prev = encState[e] & 3;
                if (encNow[e] != prev) {
                    encAcc[e] += quad[(prev << 2) | encNow[e]];
                    encState[e] = encNow[e];
                    if (encAcc[e] >= 4 || encAcc[e] <= -4) {  // EC11: 4 transitions per detent
                        post(sink_, {Event::Turn, e, encAcc[e] > 0 ? 1 : -1});
                        encAcc[e] = 0;
                    }
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
#endif
}

}  // namespace mini

#include "ps2/system.hpp"
#include "ps2/hd.hpp"

#ifdef PS2_WINDOW
#include "window.hpp"
#endif
#ifdef PS2_PROFILE
#include "profile.hpp"
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <filesystem>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "../runtime/src/third_party/stb_image.h"

void register_generated_code();
void register_generated_vu();

namespace {

int usage() {
    std::fprintf(stderr,
                 "usage: ps2run <elf> [--arg text]... [--frames n] [--ntsc] [--misses file]\n"
                 "              [--disc image.iso] [--memcard card.ps2] [--pal | --ntsc] [--trace-syscalls] [--trace-io] [--trace-threads]\n"
                 "              [--trace-iop] [--trace-iop-hle] [--frames-dir dir] [--frame-every n]\n"
                 "              [--press frame:button[+button][:frames]]... [--window] [--fullscreen]\n"
                 "              [--save-state frame:file] [--load-state file] [--widescreen] [--patches file]\n");
    return 2;
}

}

int main(int argc, char **argv) {
    if (argc < 2) return usage();
    std::vector<std::string> args{argv[1]};
    [[maybe_unused]] bool window = false, fullscreen = false;
    std::string load_path;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--arg" && i + 1 < argc) args.push_back(argv[++i]);
        else if (a == "--frames" && i + 1 < argc) ps2::g_options.max_frames = std::strtoull(argv[++i], nullptr, 10);
        else if (a == "--ntsc" || a == "--pal") {
            ps2::g_options.pal = a == "--pal";
            ps2::g_options.rate_forced = true;
        }
        else if (a == "--misses" && i + 1 < argc) ps2::g_options.misses_path = argv[++i];
        else if (a == "--trace-syscalls") ps2::g_options.trace_syscalls = true;
        else if (a == "--widescreen") ps2::g_options.widescreen = true;
        else if (a == "--aspect" && i + 1 < argc) {
            float w = 4.0f, h = 3.0f;
            if (std::sscanf(argv[++i], "%f:%f", &w, &h) == 2 && w > 0.0f && h > 0.0f) {
                ps2::g_options.aspect = w / h;
                ps2::g_options.widescreen = w / h > 1.34f;
            }
        } else if (a == "--skip-intro") {
            ps2::g_options.skip_intro = true;
        } else if (a == "--patch" && i + 1 < argc) {
            unsigned place = 0, bytes = 0;
            unsigned long address = 0;
            unsigned long long value = 0;
            if (std::sscanf(argv[++i], "%u:%lx:%u:%llx", &place, &address, &bytes, &value) == 4)
                ps2::g_options.patches.push_back({static_cast<std::uint8_t>(place), static_cast<std::uint8_t>(bytes), static_cast<std::uint32_t>(address), value});
        }
        else if (a == "--patches" && i + 1 < argc) {
            if (!ps2::load_patches(argv[++i])) return usage();
        }
        else if (a == "--trace-io") ps2::g_options.trace_io = true;
        else if (a == "--trace-threads") ps2::g_options.trace_threads = true;
        else if (a == "--disc" && i + 1 < argc) ps2::g_options.disc_path = argv[++i];
        else if (a == "--memcard" && i + 1 < argc) ps2::g_options.memcard_path = argv[++i];
        else if (a == "--trace-iop") ps2::g_options.trace_iop_modules = true;
        else if (a == "--trace-iop-hle") ps2::g_options.trace_iop_hle = true;
        else if (a == "--frames-dir" && i + 1 < argc) ps2::g_options.frames_dir = argv[++i];
        else if (a == "--press" && i + 1 < argc) {
            ps2::Press press;
            if (!ps2::parse_press(argv[++i], press)) return usage();
            ps2::g_options.presses.push_back(press);
        }
        else if (a == "--axis" && i + 1 < argc) {
            ps2::AxisPress press;
            if (!ps2::parse_axis(argv[++i], press)) return usage();
            ps2::g_options.axis_presses.push_back(press);
        }
#ifdef PS2_WINDOW
        else if (a == "--window") window = true;
        else if (a == "--fullscreen") window = fullscreen = true;
#endif
        else if (a == "--frame-every" && i + 1 < argc) ps2::g_options.frame_every = static_cast<std::uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        else if (a == "--save-state" && i + 1 < argc) {
            const std::string spec = argv[++i];
            const auto colon = spec.find(':');
            if (colon == std::string::npos) return usage();
            ps2::g_options.save_state_frame = std::strtoull(spec.substr(0, colon).c_str(), nullptr, 10);
            ps2::g_options.save_state_path = spec.substr(colon + 1);
        }
        else if (a == "--load-state" && i + 1 < argc) load_path = argv[++i];
        else return usage();
    }

#ifdef PS2_WINDOW
    if (window && !open_window(fullscreen)) return 1;
#endif
    register_generated_code();
    register_generated_vu();
    const auto emulate = [&]() -> int {
        std::uint32_t entry = 0;
        if (!ps2::load_elf(argv[1], entry)) {
            std::fprintf(stderr, "cannot load %s\n", argv[1]);
            return 1;
        }
        ps2::boot(entry, args);
        if (!load_path.empty() && !ps2::load_state(load_path)) return 1;
        if (const char *hd_dir = std::getenv("PS2_HD_DIR")) {
            // PCSX2 texture pack folder (…/textures/<serial>/replacements); PNGs are decoded on request.
            static std::string dir = hd_dir;
            std::string names;
            std::error_code ec;
            for (const auto &e : std::filesystem::directory_iterator(dir, ec))
                if (e.is_regular_file() && e.path().extension() == ".png") names += e.path().filename().string() + "\n";
            const std::size_t usable = ps2::hd_setup(
                [](const std::string &file, std::uint32_t id, std::uint32_t tw, std::uint32_t th) {
                    int w = 0, h = 0, n = 0;
                    unsigned char *px = stbi_load((dir + "/" + file).c_str(), &w, &h, &n, 4);
                    std::uint32_t shift = 0;
                    while (shift < 3 && (tw << (shift + 1)) <= static_cast<std::uint32_t>(w)) ++shift;
                    const bool exact = px && (tw << shift) == static_cast<std::uint32_t>(w) && (th << shift) == static_cast<std::uint32_t>(h);
                    if (!exact) std::fprintf(stderr, "[hd] %s: %dx%d is not %ux%u << n, skipped\n", file.c_str(), w, h, tw, th);
                    ps2::hd_loaded(id, exact ? px : nullptr, static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h), shift);
                    stbi_image_free(px);
                },
                names.data(), names.size());
            std::fprintf(stderr, "[hd] %zu replacements from %s\n", usable, hd_dir);
        }
#ifdef PS2_PROFILE
        const char *profile = std::getenv("PS2_PROFILE");
        const char *profile_from = std::getenv("PS2_PROFILE_FROM");
        if (profile) start_profile(profile_from ? std::strtoull(profile_from, nullptr, 10) : 0);
        const int result = ps2::run();
        if (profile) write_profile(profile);
        return result;
#else
        return ps2::run();
#endif
    };
#ifdef PS2_WINDOW
    if (window) return run_window(emulate);
#endif
    return emulate();
}

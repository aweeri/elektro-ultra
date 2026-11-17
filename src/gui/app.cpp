#include "gui/app.h"

#include <sys/stat.h>

#include <algorithm>
#include <cstdarg>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "imgui.h"

#include "core/common.h"
#include "core/fft.h"
#include "core/io.h"
#include "dsp/demod.h"
#include "fec/fec_chain.h"
#include "gui/sw_render.h"
#include "xrit/products.h"
#include "xrit/transport.h"

namespace eu {

namespace {

struct PanelState {
    std::string file;
    std::string type;
    double fs = 0;
    double baud = 0;
    const char* mod = "?";
    DemodStats d;
    FecStats f;
    std::vector<float> spectrum_db;   // 512 bins
    double spec_lo = 0, spec_hi = 0;
    double seconds_done = 0;
    uint64_t pngs = 0;
};

void text_dim(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.62f, 0.65f, 0.70f, 1.0f));
    ImGui::TextV(fmt, ap);
    ImGui::PopStyleColor();
    va_end(ap);
}

ImU32 lock_colour(bool ok, bool halfway) {
    if (ok) return IM_COL32(80, 200, 120, 255);
    if (halfway) return IM_COL32(230, 180, 60, 255);
    return IM_COL32(200, 80, 80, 255);
}

void chip(const char* label, bool ok, bool halfway) {
    ImGui::PushStyleColor(ImGuiCol_Button, lock_colour(ok, halfway));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, lock_colour(ok, halfway));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, lock_colour(ok, halfway));
    ImGui::Button(label, ImVec2(0, 0));
    ImGui::PopStyleColor(3);
}

void draw_constellation(const DemodChain& demod) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ImVec2 sz = ImGui::GetContentRegionAvail();
    const float side = std::min(sz.x, sz.y);
    const ImVec2 c(p.x + side * 0.5f, p.y + side * 0.5f);
    const float r = side * 0.46f;

    dl->AddRectFilled(p, ImVec2(p.x + side, p.y + side), IM_COL32(14, 16, 20, 255), 6.0f);
    dl->AddRect(p, ImVec2(p.x + side, p.y + side), IM_COL32(60, 66, 78, 255), 6.0f);

    dl->AddLine(ImVec2(c.x - r, c.y), ImVec2(c.x + r, c.y), IM_COL32(50, 56, 66, 255));
    dl->AddLine(ImVec2(c.x, c.y - r), ImVec2(c.x, c.y + r), IM_COL32(50, 56, 66, 255));
    dl->AddCircle(c, r * 0.5f, IM_COL32(44, 50, 60, 255), 64);

    const auto& pts = demod.constellation();
    // High percentile: outliers must not shrink the cloud.
    double mag = 1e-9;
    for (const auto& v : pts) mag = std::max(mag, (double)std::abs(v));
    if (mag <= 1e-9) mag = 1.0;
    const float k = r * 0.85f / (float)mag;
    for (const auto& v : pts) {
        const float x = c.x + v.real() * k;
        const float y = c.y - v.imag() * k;
        dl->AddRectFilled(ImVec2(x - 1, y - 1), ImVec2(x + 1, y + 1),
                          IM_COL32(90, 200, 240, 110));
    }
    ImGui::Dummy(ImVec2(side, side));
}

void draw_spectrum(const PanelState& st) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ImVec2 sz = ImGui::GetContentRegionAvail();
    const float h = std::max(90.0f, sz.y - 4);
    const float w = sz.x;

    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), IM_COL32(14, 16, 20, 255), 6.0f);
    if (st.spectrum_db.size() < 2) {
        ImGui::Dummy(ImVec2(w, h));
        return;
    }
    float lo = 1e9f, hi = -1e9f;
    for (float v : st.spectrum_db) {
        lo = std::min(lo, v);
        hi = std::max(hi, v);
    }
    lo = hi - 60.0f;
    const float dx = w / (float)(st.spectrum_db.size() - 1);
    ImVec2 prev(p.x, p.y + h);
    for (size_t i = 0; i < st.spectrum_db.size(); ++i) {
        const float v = std::clamp(st.spectrum_db[i], lo, hi);
        const float y = p.y + h - (v - lo) / (hi - lo) * (h - 6) - 3;
        const ImVec2 cur(p.x + dx * (float)i, y);
        if (i) dl->AddLine(prev, cur, IM_COL32(90, 200, 240, 220), 1.2f);
        prev = cur;
    }
    dl->AddLine(ImVec2(p.x + w * 0.5f, p.y), ImVec2(p.x + w * 0.5f, p.y + h),
                IM_COL32(60, 66, 78, 180));
    ImGui::Dummy(ImVec2(w, h));
}

void compute_spectrum(PanelState& st, const std::vector<cf>& x, size_t n, double fs) {
    const size_t N = 2048;
    if (n < N) return;
    Fft fft(N);
    std::vector<cf> buf(N);
    std::vector<double> acc(N, 0.0);
    const auto w = hann(N);
    size_t segs = std::min<size_t>(16, n / N);
    for (size_t s = 0; s < segs; ++s) {
        for (size_t i = 0; i < N; ++i) buf[i] = x[s * N + i] * w[i];
        fft.forward(buf.data());
        for (size_t i = 0; i < N; ++i) acc[i] += std::norm(buf[i]);
    }
    // FFT-shift: DC in the middle, like a receiver display.
    fft.fftshift(acc.data());
    st.spectrum_db.resize(N);
    for (size_t i = 0; i < N; ++i)
        st.spectrum_db[i] = (float)(10.0 * std::log10(acc[i] / (double)segs + 1e-20));
    st.spec_lo = -fs * 0.5;
    st.spec_hi = fs * 0.5;
}

}  // namespace

int run_gui(const Options& o) {
    if (o.inputs.empty()) {
        std::fprintf(stderr, "gui: need -i FILE\n");
        return 4;
    }
    std::string err;
    auto src = SampleSource::open(o.inputs.front(), o.fmt, o.rate, o.center, &err);
    if (!src) {
        std::fprintf(stderr, "gui: %s\n", err.c_str());
        return 4;
    }
    const double fs = src->info().sample_rate;

    PanelState st;
    st.file = o.inputs.front();
    st.fs = fs;
    st.type = o.type;

    std::vector<cf> block(1u << 22);   // 4M samples: a few seconds, enough to reach frame sync
    size_t got = src->read(block.data(), block.size());
    if (got < 32768) {
        std::fprintf(stderr, "gui: file too short\n");
        return 4;
    }
    compute_spectrum(st, block, got, fs);

    DemodConfig dc;
    dc.mod = (o.type == "hrit") ? Mod::Oqpsk : Mod::Bpsk;
    dc.baud = o.baud > 0 ? o.baud : (o.type == "hrit" ? kHritBaud : kLritBaud);
    st.baud = dc.baud;
    st.mod = mod_name(dc.mod);

    DemodChain demod;
    if (!demod.configure(dc, fs, &err)) {
        std::fprintf(stderr, "gui: %s\n", err.c_str());
        return 4;
    }
    FecChain fec;
    fec.configure(o.cadu_bits, o.rs_depth, o.derand, true, o.erasure);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)(o.gui_scale > 0 ? 1280 * o.gui_scale : 1280),
                            (float)(o.gui_scale > 0 ? 820 * o.gui_scale : 820));
    io.IniFilename = nullptr;
    io.Fonts->AddFontDefault();
    io.Fonts->Build();

    unsigned char* tex = nullptr;
    int tw = 0, th = 0;
    io.Fonts->GetTexDataAsRGBA32(&tex, &tw, &th);

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 0.0f;
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.055f, 0.063f, 0.078f, 1.0f);
    style.Colors[ImGuiCol_ChildBg] = ImVec4(0.075f, 0.086f, 0.104f, 1.0f);
    style.Colors[ImGuiCol_Border] = ImVec4(0.16f, 0.18f, 0.22f, 1.0f);

    Canvas canvas;
    canvas.resize((int)io.DisplaySize.x, (int)io.DisplaySize.y);

    std::vector<int8_t> soft;
    std::vector<std::vector<uint8_t>> payloads;
    TransportDemux transport;
    ProductWriter products;
    ProductStats pstats;
    std::vector<XritBlob> blobs;
    products.configure(o.out.empty() ? "out/gui" : o.out, o.type, false);

    // Prime so frame 1 already shows a locked constellation.
    size_t cursor = 0;
    {
        const size_t prime = std::min<size_t>(got, 640000);
        soft.clear();
        demod.process(block.data(), prime, soft);
        if (!soft.empty()) {
            payloads.clear();
            fec.push(soft.data(), soft.size(), payloads);
            for (const auto& p : payloads) {
                blobs.clear();
                transport.push(p.data(), p.size(), blobs);
                for (const auto& b : blobs) products.push(b, pstats);
            }
        }
        cursor = prime;
    }

    const int frames = o.gui_frames > 0 ? o.gui_frames : 1;
    int frames_done = 0;

    while (frames_done < frames) {
        if (cursor < got) {
            const size_t take = std::min<size_t>(got - cursor, 65536);
            soft.clear();
            demod.process(block.data() + cursor, take, soft);
            cursor += take;
            if (!soft.empty()) {
                payloads.clear();
                fec.push(soft.data(), soft.size(), payloads);
                for (const auto& p : payloads) {
                    blobs.clear();
                    transport.push(p.data(), p.size(), blobs);
                    for (const auto& b : blobs) products.push(b, pstats);
                }
            }
        }
        st.d = demod.stats();
        st.f = fec.stats();
        st.seconds_done = (double)cursor / fs;
        st.pngs = pstats.pngs;

        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::Begin("elektro-ultra", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar);

        ImGui::TextUnformatted("elektro-ultra");
        ImGui::SameLine();
        text_dim("%s  %s  %.0f Bd  %.3f MSPS", st.type.c_str(), st.mod, st.baud,
                 st.fs / 1e6);
        ImGui::SameLine();
        chip(st.d.locked ? "demod locked" : "demod", st.d.locked, st.d.evm_pct < 60);
        ImGui::SameLine();
        chip(st.f.deframer_locked ? "frame sync" : "no sync", st.f.deframer_locked,
             st.f.viterbi_locked);
        ImGui::SameLine();
        chip(st.f.cadus_good > 0 ? "decoding" : "idle", st.f.cadus_good > 0, st.f.cadus > 0);

        ImGui::Separator();

        const float left_w = io.DisplaySize.x * 0.34f;
        ImGui::BeginChild("left", ImVec2(left_w, 0), true);
        ImGui::TextUnformatted("demodulator");
        ImGui::Separator();
        ImGui::Text("snr       %.1f dB", st.d.snr_db);
        ImGui::Text("evm       %.1f %%", st.d.evm_pct);
        ImGui::Text("carrier   %+.0f Hz", st.d.carrier_hz);
        ImGui::Text("residual  %+.0f Hz", st.d.freq_err_hz);
        ImGui::Text("clock     %+.1f ppm", st.d.clock_ppm);
        ImGui::Text("timing    %.2f samp", st.d.timing_phase);
        ImGui::Text("symbols   %llu", (unsigned long long)st.d.symbols);
        ImGui::Spacing();
        ImGui::TextUnformatted("fec");
        ImGui::Separator();
        ImGui::Text("viterbi   ber %.4f  %s", st.f.viterbi_ber,
                    st.f.viterbi_locked ? "locked" : "-");
        ImGui::Text("frames    %llu / %llu", (unsigned long long)st.f.cadus_good,
                    (unsigned long long)st.f.cadus);
        ImGui::Text("rs fixed  %llu", (unsigned long long)st.f.rs_corrected);
        ImGui::Text("rs failed %llu", (unsigned long long)st.f.rs_failed);
        ImGui::Text("polarity  %llu flips", (unsigned long long)st.f.polarity_flips);
        ImGui::Spacing();
        ImGui::TextUnformatted("products");
        ImGui::Separator();
        ImGui::Text("packets   %llu", (unsigned long long)transport.packets());
        ImGui::Text("xrit      %llu files", (unsigned long long)pstats.files_seen);
        ImGui::Text("images    %llu (png %llu)", (unsigned long long)pstats.images,
                    (unsigned long long)pstats.pngs);
        ImGui::Text("position  %.2f s", st.seconds_done);
        ImGui::EndChild();

        ImGui::SameLine();
        ImGui::BeginChild("right", ImVec2(0, 0), false);
        ImGui::BeginChild("const", ImVec2(0, io.DisplaySize.y * 0.52f), true);
        draw_constellation(demod);
        ImGui::EndChild();
        ImGui::BeginChild("spec", ImVec2(0, 0), true);
        ImGui::TextUnformatted("spectrum (baseband, DC centred)");
        draw_spectrum(st);
        ImGui::EndChild();
        ImGui::EndChild();

        ImGui::End();
        ImGui::Render();

        canvas.clear(14, 16, 20, 255);
        render_draw_data(ImGui::GetDrawData(), canvas, tex, tw, th);

        if (!o.gui_out.empty()) {
            char path[512];
            std::snprintf(path, sizeof path, o.gui_out.c_str(), frames_done);
            if (canvas_to_png(canvas, path)) ++st.pngs;
        }
        ++frames_done;

        if (cursor >= got && frames > 1) {
        }
    }

    if (o.gui_out.empty()) {
        std::string path = (o.out.empty() ? std::string("out/gui") : o.out) + "/viewer.png";
        ::mkdir((o.out.empty() ? "out" : o.out).c_str(), 0755);
        ::mkdir((o.out.empty() ? "out/gui" : o.out).c_str(), 0755);
        canvas_to_png(canvas, path.c_str());
        std::printf("gui: wrote %s\n", path.c_str());
    }
    products.flush(pstats);

    ImGui::DestroyContext();
    std::printf("gui: %d frame(s), %.2f s of IQ processed\n", frames_done, st.seconds_done);
    return 0;
}

}  // namespace eu

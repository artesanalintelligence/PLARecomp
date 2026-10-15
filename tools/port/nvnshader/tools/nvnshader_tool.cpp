// SPDX-License-Identifier: GPL-3.0-or-later
//
// nvnshader_tool: offline NVN -> SPIR-V shader translation with cache and validation.
//
//   nvnshader_tool translate <extract_dir> [--cache DIR] [--report FILE] [--jobs N]
//                  [--no-validate] [--hints FILE] [--dump-spv DIR] [--limit N] [--only HASH]
//                  [--list FILE]   (only the hashes in FILE's first column)
//                  [--states FILE] (one translation per line "hash key=value..."; see ParseStateLine)
//   nvnshader_tool rebind <extract_dir> --hints FILE   (TranslateBound test)
//   nvnshader_tool info <file.ctrl>
//
// <extract_dir>/shaders/<hash>.ctrl + .data are produced by scripts/romfs_scan.py.
// --hints: lines "<hash> <cbuf> <offset> <type> <integer>" (scripts/hints_from_suyu.py).

#include <algorithm>
#include <array>
#include <optional>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <spirv-tools/libspirv.h>

#include "nvnshader/nvnshader.h"
#include "state_line.h"

namespace fs = std::filesystem;
using namespace nvnshader;
using nvnshader::tools::ParseStateLine;

namespace {

std::vector<uint8_t> ReadFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

std::string JsonEscape(const std::string& s) {
    std::string o;
    for (char c : s) {
        switch (c) {
        case '"':
            o += "\\\"";
            break;
        case '\\':
            o += "\\\\";
            break;
        case '\n':
            o += "\\n";
            break;
        case '\r':
            break;
        case '\t':
            o += "\\t";
            break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                o += '?';
            } else {
                o += c;
            }
        }
    }
    return o;
}

const char* StageName(Stage s) {
    switch (s) {
    case Stage::Vertex:
        return "vertex";
    case Stage::Fragment:
        return "fragment";
    case Stage::Geometry:
        return "geometry";
    case Stage::TessControl:
        return "tess_control";
    case Stage::TessEval:
        return "tess_eval";
    case Stage::Compute:
        return "compute";
    }
    return "?";
}

struct Item {
    std::string hash;
    fs::path ctrl;
    fs::path data;
    bool has_state = false;
    TranslationState state{};
    std::string tag; ///< identifier of the --states line
};


struct Outcome {
    std::string hash;
    Stage stage{};
    bool translated = false;
    bool from_cache = false;
    bool valid = false;
    bool validated = false;
    std::string error;
    std::string val_error;
    std::string warnings;
    size_t spirv_words = 0;
    size_t descriptors = 0;
    std::string desc_summary;
    double ms = 0;
};

class Validator {
public:
    Validator() {
        ctx = spvContextCreate(SPV_ENV_VULKAN_1_3);
        opts = spvValidatorOptionsCreate();
        spvValidatorOptionsSetScalarBlockLayout(opts, true);
    }
    ~Validator() {
        spvValidatorOptionsDestroy(opts);
        spvContextDestroy(ctx);
    }
    bool Validate(const std::vector<uint32_t>& words, std::string* error) {
        spv_const_binary_t bin{words.data(), words.size()};
        spv_diagnostic diag = nullptr;
        const spv_result_t r = spvValidateWithOptions(ctx, opts, &bin, &diag);
        if (r != SPV_SUCCESS && error) {
            *error = diag && diag->error ? diag->error : "spirv-val: error without diagnostic";
        }
        spvDiagnosticDestroy(diag);
        return r == SPV_SUCCESS;
    }

private:
    spv_context ctx;
    spv_validator_options opts;
};

std::string DescSummary(const ShaderModule& m) {
    std::ostringstream o;
    for (const Descriptor& d : m.descriptors) {
        static const char* kNames[] = {"ubo", "ssbo", "tbuf", "ibuf", "tex", "img"};
        o << kNames[static_cast<int>(d.type)] << "(c" << d.cbuf_index;
        if (d.type != DescriptorType::UniformBuffer) {
            o << "+0x" << std::hex << d.cbuf_offset << std::dec;
        } else {
            o << "," << d.size;
        }
        if (d.count > 1) {
            o << "x" << d.count;
        }
        o << ") ";
    }
    return o.str();
}

int CmdInfo(const fs::path& ctrl_path) {
    const auto ctrl = ReadFile(ctrl_path);
    auto data_path = ctrl_path;
    data_path.replace_extension(".data");
    const auto data = ReadFile(data_path);
    ShaderBinary b{ctrl.data(), ctrl.size(), data.empty() ? nullptr : data.data(), data.size()};
    ShaderHeader h{};
    std::string err;
    if (!ParseControl(b, &h, &err)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }
    std::printf("stage %s, header 0x%x, code 0x%x, c1 0x%x+0x%x, data 0x%x, regs %u, "
                "wg %ux%ux%u, shared 0x%x, hash %016llx\n",
                StageName(h.stage), h.code_offset, h.code_size, h.const_offset, h.const_size,
                h.data_size, h.num_registers, h.workgroup_size[0], h.workgroup_size[1],
                h.workgroup_size[2], h.shared_memory_size,
                static_cast<unsigned long long>(h.code_hash));
    return 0;
}

int CmdTranslate(int argc, char** argv) {
    fs::path extract = argv[2];
    std::string cache_dir;
    std::string report = "nvnshader_report.json";
    std::string hints_file;
    std::string dump_dir;
    std::string only;
    std::string states_file;
    OptLevel opt = OptLevel::None;
    std::set<std::string> only_set;
    unsigned jobs = std::max(1u, std::thread::hardware_concurrency() - 2);
    bool validate = true;
    size_t limit = 0;
    for (int i = 3; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--cache") {
            cache_dir = next();
        } else if (a == "--report") {
            report = next();
        } else if (a == "--jobs") {
            jobs = static_cast<unsigned>(std::stoul(next()));
        } else if (a == "--no-validate") {
            validate = false;
        } else if (a == "--hints") {
            hints_file = next();
        } else if (a == "--dump-spv") {
            dump_dir = next();
        } else if (a == "--limit") {
            limit = std::stoul(next());
        } else if (a == "--only") {
            only = next();
        } else if (a == "--opt") {
            const std::string v = next();
            opt = v == "Os" ? OptLevel::Size : v == "O" ? OptLevel::Performance : OptLevel::None;
        } else if (a == "--states") {
            states_file = next();
        } else if (a == "--list") {
            std::ifstream lf(next());
            std::string h;
            while (lf >> h) {
                only_set.insert(h);
                std::string rest;
                std::getline(lf, rest);
            }
        }
    }
    std::vector<Item> items;
    const fs::path shaders_dir = fs::exists(extract / "shaders") ? extract / "shaders" : extract;
    for (const auto& e : fs::directory_iterator(shaders_dir)) {
        if (e.path().extension() != ".ctrl") {
            continue;
        }
        Item it;
        it.hash = e.path().stem().string();
        if (!only.empty() && it.hash != only) {
            continue;
        }
        if (!only_set.empty() && !only_set.count(it.hash)) {
            continue;
        }
        it.ctrl = e.path();
        it.data = e.path();
        it.data.replace_extension(".data");
        items.push_back(std::move(it));
    }
    std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.hash < b.hash; });
    if (!states_file.empty()) {
        // One translation per line (hash + state) instead of one per shader.
        std::vector<Item> by_state;
        std::ifstream sf(states_file);
        std::string line;
        while (std::getline(sf, line)) {
            if (line.empty() || line[0] == '#') {
                continue;
            }
            Item it;
            if (!ParseStateLine(line, it.hash, it.state, it.tag)) {
                continue;
            }
            it.ctrl = shaders_dir / (it.hash + ".ctrl");
            it.data = shaders_dir / (it.hash + ".data");
            it.has_state = true;
            by_state.push_back(std::move(it));
        }
        items = std::move(by_state);
    }
    if (limit && items.size() > limit) {
        items.resize(limit);
    }
    std::unordered_map<std::string, std::vector<TextureHint>> hints;
    if (!hints_file.empty()) {
        std::ifstream hf(hints_file);
        std::string h;
        uint32_t ci, off, type, integer;
        while (hf >> h >> ci >> off >> type >> integer) {
            hints[h].push_back(TextureHint{ci, off, static_cast<TextureType>(type), integer != 0});
        }
    }
    if (!dump_dir.empty()) {
        fs::create_directories(dump_dir);
    }
    std::optional<DiskCache> cache;
    if (!cache_dir.empty()) {
        cache.emplace(cache_dir);
    }
    const HostCaps caps{};
    std::vector<Outcome> outcomes(items.size());
    std::atomic<size_t> next_index{0};
    std::atomic<size_t> done{0};
    std::mutex print_mtx;
    const auto t0 = std::chrono::steady_clock::now();
    auto worker = [&] {
        Validator validator;
        for (;;) {
            const size_t i = next_index.fetch_add(1);
            if (i >= items.size()) {
                return;
            }
            const Item& it = items[i];
            Outcome& out = outcomes[i];
            out.hash = it.hash;
            const auto ctrl = ReadFile(it.ctrl);
            const auto data = ReadFile(it.data);
            ShaderBinary b{ctrl.data(), ctrl.size(), data.data(), data.size()};
            ShaderHeader header{};
            std::string err;
            if (!ParseControl(b, &header, &err)) {
                out.error = "control: " + err;
                ++done;
                continue;
            }
            out.stage = header.stage;
            TranslationState state = it.has_state ? it.state : TranslationState{};
            TranslateOptions options{};
            options.optimize = opt;
            if (auto h = hints.find(it.hash); h != hints.end() && state.texture_hints.empty()) {
                state.texture_hints = h->second;
            }
            const uint64_t key = CacheKey(header, state, options, caps);
            TranslateResult result;
            const auto ts = std::chrono::steady_clock::now();
            if (cache && cache->Load(key, &result)) {
                out.translated = true;
                out.from_cache = true;
            } else if (Translate(b, state, caps, options, &result, &err)) {
                out.translated = true;
                if (cache) {
                    cache->Store(key, result);
                }
            } else {
                out.error = err;
            }
            out.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - ts).count();
            if (out.translated) {
                const ShaderModule& m = result.module;
                out.spirv_words = m.spirv.size();
                out.descriptors = m.descriptors.size();
                out.desc_summary = DescSummary(m);
                out.warnings = m.warnings;
                if (validate) {
                    out.validated = true;
                    out.valid = validator.Validate(m.spirv, &out.val_error);
                }
                if (!dump_dir.empty()) {
                    const std::string suffix = it.tag.empty() ? "" : "." + it.tag;
                    std::ofstream f(fs::path(dump_dir) / (it.hash + suffix + "." + StageName(m.stage) + ".spv"),
                                    std::ios::binary);
                    f.write(reinterpret_cast<const char*>(m.spirv.data()),
                            static_cast<std::streamsize>(m.spirv.size() * 4));
                }
            }
            const size_t d = ++done;
            if (d % 500 == 0) {
                std::scoped_lock lk{print_mtx};
                std::printf("  %zu/%zu\n", d, items.size());
                std::fflush(stdout);
            }
        }
    };
    std::vector<std::thread> threads;
    for (unsigned j = 0; j < jobs; ++j) {
        threads.emplace_back(worker);
    }
    for (auto& t : threads) {
        t.join();
    }
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    std::map<std::string, std::array<size_t, 5>> by_stage; // total, ok, valid, cache, warn
    std::map<std::string, size_t> errors;
    std::map<std::string, size_t> val_errors;
    for (const Outcome& o : outcomes) {
        auto& s = by_stage[StageName(o.stage)];
        s[0]++;
        s[1] += o.translated;
        s[2] += o.valid;
        s[3] += o.from_cache;
        s[4] += !o.warnings.empty();
        if (!o.translated) {
            errors[o.error.substr(0, 120)]++;
        } else if (o.validated && !o.valid) {
            val_errors[o.val_error.substr(0, 160)]++;
        }
    }
    size_t total = 0, ok = 0, valid = 0, cached = 0;
    std::printf("\n%-10s %7s %7s %7s %7s %7s\n", "stage", "total", "transl", "valid", "cache", "warns");
    for (const auto& [name, s] : by_stage) {
        std::printf("%-10s %7zu %7zu %7zu %7zu %7zu\n", name.c_str(), s[0], s[1], s[2], s[3], s[4]);
        total += s[0];
        ok += s[1];
        valid += s[2];
        cached += s[3];
    }
    std::printf("%-10s %7zu %7zu %7zu %7zu\n", "TOTAL", total, ok, valid, cached);
    std::printf("time %.1f s with %u threads\n", secs, jobs);
    if (!errors.empty()) {
        std::printf("\ntranslation errors:\n");
        for (const auto& [e, n] : errors) {
            std::printf("  %5zu  %s\n", n, e.c_str());
        }
    }
    if (!val_errors.empty()) {
        std::printf("\nspirv-val errors:\n");
        for (const auto& [e, n] : val_errors) {
            std::printf("  %5zu  %s\n", n, e.c_str());
        }
    }
    std::ofstream rf(report);
    rf << "{\n \"total\": " << total << ", \"translated\": " << ok << ", \"valid\": " << valid
       << ", \"from_cache\": " << cached << ", \"seconds\": " << secs << ",\n \"shaders\": [\n";
    for (size_t i = 0; i < outcomes.size(); ++i) {
        const Outcome& o = outcomes[i];
        rf << "  {\"hash\": \"" << o.hash << "\", \"stage\": \"" << StageName(o.stage)
           << "\", \"translated\": " << (o.translated ? "true" : "false")
           << ", \"valid\": " << (o.valid ? "true" : "false") << ", \"words\": " << o.spirv_words
           << ", \"ms\": " << o.ms << ", \"descriptors\": \"" << JsonEscape(o.desc_summary)
           << "\", \"error\": \"" << JsonEscape(o.error) << "\", \"val_error\": \""
           << JsonEscape(o.val_error) << "\", \"warnings\": \"" << JsonEscape(o.warnings.substr(0, 400))
           << "\"}" << (i + 1 < outcomes.size() ? "," : "") << "\n";
    }
    rf << " ]\n}\n";
    return ok == total && (!validate || valid == total) ? 0 : 2;
}


// TranslateBound test: for each shader in --hints, the "backend" binds the textures the hint
// describes (real type/integer) and requests the variant. Checks that the result matches a
// direct translation with those hints and that the second request is served from the memory cache.
int CmdRebind(int argc, char** argv) {
    fs::path extract = argv[2];
    std::string hints_file;
    for (int i = 3; i < argc; ++i) {
        if (std::string(argv[i]) == "--hints" && i + 1 < argc) {
            hints_file = argv[++i];
        }
    }
    const fs::path dir = fs::exists(extract / "shaders") ? extract / "shaders" : extract;
    std::map<std::string, std::vector<TextureHint>> hints;
    {
        std::ifstream hf(hints_file);
        std::string h;
        uint32_t ci, off, type, integer;
        while (hf >> h >> ci >> off >> type >> integer) {
            hints[h].push_back(TextureHint{ci, off, static_cast<TextureType>(type), integer != 0});
        }
    }
    const HostCaps caps{};
    auto tr = CreateTranslator(caps, "");
    size_t total = 0, changed = 0, equal = 0, matched = 0, second_hit = 0, failed = 0, redundant = 0;
    double ms_first = 0, ms_second = 0;
    for (const auto& [hash, hs] : hints) {
        const auto ctrl = ReadFile(dir / (hash + ".ctrl"));
        const auto data = ReadFile(dir / (hash + ".data"));
        ShaderBinary b{ctrl.data(), ctrl.size(), data.data(), data.size()};
        TextureResolver resolver = [&hs](uint32_t ci, uint32_t off, TextureHint* out) {
            for (const TextureHint& h : hs) {
                if (h.cbuf_index == ci && h.cbuf_offset == off) {
                    *out = h;
                    return true;
                }
            }
            return false;
        };
        ++total;
        std::string err;
        ShaderModule plain;
        if (!tr->Translate(b, TranslationState{}, &plain, &err)) {
            ++failed;
            continue;
        }
        auto t0 = std::chrono::steady_clock::now();
        auto m = tr->TranslateBound(b, TranslationState{}, resolver, &err);
        auto t1 = std::chrono::steady_clock::now();
        auto m2 = tr->TranslateBound(b, TranslationState{}, resolver, &err);
        auto t2 = std::chrono::steady_clock::now();
        ms_first += std::chrono::duration<double, std::milli>(t1 - t0).count();
        ms_second += std::chrono::duration<double, std::milli>(t2 - t1).count();
        if (!m) {
            ++failed;
            std::printf("  %s: %s\n", hash.c_str(), err.c_str());
            continue;
        }
        second_hit += m2 == m;
        matched += TextureQueriesMatch(*m, resolver);
        TranslationState direct{};
        direct.texture_hints = hs;
        ShaderModule ref;
        tr->Translate(b, direct, &ref, &err);
        equal += ref.spirv == m->spirv;
        // Default variant (resolver that knows nothing): if TranslateBound created another variant with
        // the same SPIR-V, the failed query did not change the code (a "redundant" variant).
        auto d = tr->TranslateBound(b, TranslationState{}, TextureResolver{}, &err);
        redundant += d && d != m && d->spirv == m->spirv;
        if (m->spirv != plain.spirv) {
            ++changed;
            std::printf("  new variant: %s (%s)\n", hash.c_str(), StageName(m->stage));
        }
    }
    std::printf("shaders %zu; changed by real textures %zu; match direct translation %zu; "
                "queries satisfied %zu; 2nd request from memory %zu; redundant variants %zu; failures %zu\n"
                "TranslateBound: %.2f ms average 1st time, %.4f ms 2nd\n",
                total, changed, equal, matched, second_hit, redundant, failed, ms_first / std::max<size_t>(1, total),
                ms_second / std::max<size_t>(1, total));
    return (failed == 0 && equal == total && matched == total) ? 0 : 2;
}

} // namespace

int main(int argc, char** argv) {
    if (argc >= 3 && std::string(argv[1]) == "translate") {
        return CmdTranslate(argc, argv);
    }
    if (argc >= 3 && std::string(argv[1]) == "rebind") {
        return CmdRebind(argc, argv);
    }
    if (argc >= 3 && std::string(argv[1]) == "info") {
        return CmdInfo(argv[2]);
    }
    std::fprintf(stderr,
                 "usage:\n  nvnshader_tool translate <extract_dir> [--cache DIR] [--report FILE] "
                 "[--jobs N] [--no-validate] [--hints FILE] [--dump-spv DIR] [--limit N] [--only HASH]\n"
                 "  nvnshader_tool rebind <extract_dir> --hints FILE\n"
                 "  nvnshader_tool info <file.ctrl>\n");
    return 1;
}

#include <cstdio>
#include <cstring>
#include <string>

#include "cli/options.h"
#include "core/common.h"
#include "core/io.h"

namespace eu {
int run_decode(const Options& o);
int run_probe(const Options& o);
int run_synth(const Options& o);
int run_selftest(const Options& o);
int run_bench(const Options& o);
int run_noisify(const Options& o);
}  // namespace eu

int main(int argc, char** argv) {
    using namespace eu;
    Options o;
    std::string err;
    if (!parse_options(argc, argv, o, err)) {
        std::fprintf(stderr, "elektro-ultra: %s\n", err.c_str());
        std::fprintf(stderr, "try: %s --help\n", argv[0]);
        return 4;
    }
    if (o.cmd == "help") { print_help(argv[0]); return 0; }

    if (o.quiet) Log::level = LogLevel::Warn;
    if (o.verbose) Log::level = LogLevel::Debug;
    if (o.json_stdout) Log::out = stderr;

    apply_type_preset(o);

    if (o.cmd == "decode") return run_decode(o);
    if (o.cmd == "probe") return run_probe(o);
    if (o.cmd == "synth") return run_synth(o);
    if (o.cmd == "selftest") return run_selftest(o);
    if (o.cmd == "bench") return run_bench(o);
    if (o.cmd == "noisify") return run_noisify(o);

    std::fprintf(stderr, "elektro-ultra: unknown command '%s'\n", o.cmd.c_str());
    return 4;
}

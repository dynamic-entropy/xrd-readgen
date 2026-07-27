#include <CLI/CLI.hpp>
#include <XrdCl/XrdClDefaultEnv.hh>
#include <XrdVersion.hh>

#include <cstdio>
#include <string>

#include "readgen/read_command.hh"

namespace {

const char *BuildArch() {
#if defined(__aarch64__) || defined(__arm64__)
  return "aarch64";
#elif defined(__x86_64__)
  return "x86_64";
#else
  return "unknown";
#endif
}

int NotImplemented(const char *cmd, const char *phase) {
  std::fprintf(stderr, "%s: not implemented yet (%s)\n", cmd, phase);
  return 2;
}

} // namespace

int main(int argc, char **argv) {
  // Tag all traffic so server-side monitoring (MONIT / AAA ops) can identify
  // and filter it. Equivalent to XRD_APPNAME.
  XrdCl::DefaultEnv::GetEnv()->PutString(
      "AppName", std::string("xrd-readgen/") + READGEN_VERSION);

  CLI::App app{"xrd-readgen — XRootD remote-read traffic generator"};
  app.require_subcommand(1);

  // read
  readgen::ReadOptions read_opts;
  auto *read_cmd =
      app.add_subcommand("read", "Timed single-file remote read (smoke test)");
  read_cmd->add_option("url", read_opts.url, "root:// URL of the file")
      ->required();
  read_cmd->add_option("--chunk-size", read_opts.chunk_size,
                       "Bytes per read op (default 1 MiB)");
  read_cmd->add_option("--offset", read_opts.offset, "Starting offset");
  read_cmd->add_option("--max-bytes", read_opts.max_bytes,
                       "Stop after N bytes (default: read to EOF)");
  read_cmd->add_option("--vector", read_opts.vector_chunks,
                       "Use VectorRead with N chunks per op");
  read_cmd->add_flag("--json", read_opts.json, "JSON output");

  // Phase 1+ stubs
  std::string workload;
  auto *run_cmd = app.add_subcommand("run", "Execute a workload menu");
  run_cmd->add_option("workload", workload, "workload YAML")->required();
  auto *validate_cmd =
      app.add_subcommand("validate", "Validate a workload menu");
  validate_cmd->add_option("workload", workload, "workload YAML")->required();
  auto *probe_cmd =
      app.add_subcommand("probe", "Pre-flight open+TTFB probe of a filelist");
  auto *report_cmd =
      app.add_subcommand("report", "Summarize a run from its result files");
  auto *version_cmd = app.add_subcommand("version", "Version info");

  CLI11_PARSE(app, argc, argv);

  if (read_cmd->parsed())
    return readgen::RunReadCommand(read_opts);
  if (run_cmd->parsed())
    return NotImplemented("run", "Phase 1");
  if (validate_cmd->parsed())
    return NotImplemented("validate", "Phase 1");
  if (probe_cmd->parsed())
    return NotImplemented("probe", "Phase 1");
  if (report_cmd->parsed())
    return NotImplemented("report", "Phase 1");
  if (version_cmd->parsed()) {
    std::printf("xrd-readgen %s (%s, XrdCl %s)\n", READGEN_VERSION, BuildArch(),
                XrdVERSION);
    return 0;
  }
  return 0;
}

#include <CLI/CLI.hpp>
#include <XrdCl/XrdClDefaultEnv.hh>
#include <XrdVersion.hh>

#include <cstdio>
#include <stdexcept>
#include <string>

#include "readgen/build_info.hh"
#include "readgen/probe_command.hh"
#include "readgen/read_command.hh"
#include "readgen/run_command.hh"
#include "readgen/run_config.hh"
#include "readgen/units.hh"
#include "readgen/workload_run_command.hh"
#include "readgen/workload_spec.hh"

#include <fstream>
#include <vector>

namespace {

int NotImplemented(const char* cmd, const char* note) {
    std::fprintf(stderr, "%s: not implemented yet (%s)\n", cmd, note);
    return 2;
}

readgen::PatternType ParsePattern(const std::string& s) {
    if (s == "sequential") return readgen::PatternType::Sequential;
    if (s == "random") return readgen::PatternType::Random;
    if (s == "vector") return readgen::PatternType::Vector;
    if (s == "mixed") return readgen::PatternType::Mixed;
    throw std::runtime_error("pattern must be sequential|random|vector|mixed");
}

bool AnyCounted(const std::vector<CLI::Option*>& opts) {
    for (const auto* o : opts) {
        if (o && o->count() > 0) return true;
    }
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    XrdCl::DefaultEnv::GetEnv()->PutString("AppName", std::string("xrd-readgen/") + READGEN_VERSION);

    CLI::App app{"xrd-readgen — XRootD remote-read traffic generator"};
    app.require_subcommand(1);

    // read
    readgen::ReadOptions read_opts;
    auto* read_cmd = app.add_subcommand("read", "Timed single-file remote read (smoke test)");
    read_cmd->add_option("url", read_opts.url, "root:// URL of the file")->required();
    read_cmd->add_option("--chunk-size", read_opts.chunk_size, "Bytes per read op (default 1 MiB)");
    read_cmd->add_option("--offset", read_opts.offset, "Starting offset");
    read_cmd->add_option("--max-bytes", read_opts.max_bytes, "Stop after N bytes (default: read to EOF)");
    read_cmd->add_option("--vector", read_opts.vector_chunks, "Use VectorRead with N chunks per op");
    read_cmd->add_flag("--json", read_opts.json, "JSON output");

    // run: legacy flags OR workload JSON (mutually exclusive modes)
    readgen::RunConfig run_cfg;
    std::string duration_str = "30s";
    std::string rate_str;
    std::string chunk_str = "1MiB";
    std::string max_bytes_str = "auto";
    std::string pattern_str = "sequential";
    std::string filelist_path;
    std::string snapshot_str = "15s";
    std::string session_timeout_str = "60s";
    bool no_results = false;
    std::string run_workload;
    std::string run_target;
    bool run_skip_auth = false;

    auto* run_cmd = app.add_subcommand(
        "run", "Execute a sustained read workload (CLI flags or workload JSON)");
    auto* run_workload_opt =
        run_cmd->add_option("workload", run_workload, "workload JSON (workload mode)");
    auto* endpoint_opt =
        run_cmd->add_option("--endpoint", run_cfg.endpoint,
                            "root:// endpoint (e.g. root://localhost:10945/)");
    auto* filelist_opt =
        run_cmd->add_option("--filelist", filelist_path, "File with one path per line");
    auto* duration_opt = run_cmd->add_option("--duration", duration_str, "Run duration (e.g. 30s, 5m)");
    auto* rate_opt = run_cmd->add_option("--rate", rate_str,
                                         "Target rate (prefer MBps/Gbps SI; also MiBps/Mbps); omit to uncap");
    auto* workers_opt =
        run_cmd->add_option("--workers", run_cfg.workers, "Max in-flight sessions (default 16)")
            ->check(CLI::Range(1u, 100000u));
    auto* pattern_opt = run_cmd->add_option("--pattern", pattern_str, "sequential|random|vector|mixed");
    auto* chunk_opt =
        run_cmd->add_option("--chunk-size", chunk_str, "Bytes per read chunk (e.g. 1MiB or 1MB)");
    auto* vector_chunks_opt =
        run_cmd->add_option("--vector-chunks", run_cfg.vector_chunks, "Chunks per VectorRead");
    auto* vector_fraction_opt =
        run_cmd->add_option("--vector-fraction", run_cfg.vector_fraction,
                            "Mixed pattern: fraction of sessions using VectorRead (default 0.4)")
            ->check(CLI::Range(0.0, 1.0));
    auto* file_fraction_opt =
        run_cmd->add_option("--file-fraction", run_cfg.file_fraction, "Fraction of each file to read");
    auto* max_bytes_opt = run_cmd->add_option(
        "--max-bytes", max_bytes_str,
        "Session byte cap (SIZE, 0=none, or 'auto' from --rate/--workers; default auto)");
    auto* session_timeout_opt = run_cmd->add_option(
        "--session-timeout", session_timeout_str, "Per-session wall timeout (0 to disable; default 60s)");
    auto* connection_window_opt = run_cmd->add_option(
        "--connection-window", run_cfg.connection_window_s,
        "XrdCl ConnectionWindow seconds (default 15; XrdCl default is 120)");
    auto* connection_retry_opt =
        run_cmd->add_option("--connection-retry", run_cfg.connection_retry,
                            "XrdCl ConnectionRetry count (default 2)");
    auto* request_timeout_opt =
        run_cmd->add_option("--request-timeout", run_cfg.request_timeout_s,
                            "XrdCl RequestTimeout seconds (default 60)");
    auto* seed_opt = run_cmd->add_option("--seed", run_cfg.seed, "RNG seed");
    auto* run_id_opt = run_cmd->add_option("--run-id", run_cfg.run_id, "Run identifier");
    auto* job_id_opt =
        run_cmd->add_option("--job-id", run_cfg.job_id, "Job/instance label (default: hostname)");
    auto* results_dir_opt = run_cmd->add_option("--results-dir", run_cfg.results_dir,
                                                "Directory for metrics.jsonl + result.json");
    auto* snapshot_opt =
        run_cmd->add_option("--snapshot-interval", snapshot_str, "Metrics JSONL snapshot interval");
    auto* no_results_opt = run_cmd->add_flag("--no-results", no_results, "Disable FileSink output");
    auto* pushgateway_opt = run_cmd->add_option(
        "--pushgateway", run_cfg.pushgateway_url,
        "Push metrics to Pushgateway base URL (e.g. http://xrdmon.cern.ch:9091)");
    auto* pushgateway_job_opt =
        run_cmd->add_option("--pushgateway-job", run_cfg.pushgateway_job,
                            "Pushgateway job label (default xrd-readgen)");
    auto* pushgateway_keep_opt = run_cmd->add_flag(
        "--pushgateway-keep", run_cfg.pushgateway_keep, "Do not DELETE Pushgateway group on exit");
    run_cmd->add_flag("--dry-run", run_cfg.dry_run, "Print resolved config; no I/O");
    auto* run_target_opt =
        run_cmd->add_option("--target", run_target, "Workload mode: target name (required if multi-target)");
    auto* run_skip_auth_opt = run_cmd->add_flag(
        "--skip-auth-check", run_skip_auth, "Workload mode: skip x509 proxy preflight (local only)");
    (void)run_workload_opt;

    auto* validate_cmd = app.add_subcommand("validate", "Validate a workload JSON (no XRootD I/O)");
    std::string workload;
    std::string validate_out;
    validate_cmd->add_option("workload", workload, "workload JSON")->required();
    validate_cmd->add_option("--out", validate_out, "Write canonical resolved JSON to PATH");

    readgen::ProbeOptions probe_opts;
    auto* probe_cmd = app.add_subcommand("probe", "Pre-flight Open+TTFB probe of a workload");
    probe_cmd->add_option("workload", probe_opts.workload_path, "workload JSON")->required();
    probe_cmd->add_option("--target", probe_opts.target, "Probe only this target name");
    probe_cmd->add_option("--limit", probe_opts.limit, "Max files per target (default: all)");
    probe_cmd->add_option("--concurrency", probe_opts.concurrency, "Max in-flight probes (default 4)")
        ->check(CLI::Range(1u, 100000u));
    probe_cmd->add_flag("--json", probe_opts.json, "JSON probe report on stdout");
    probe_cmd->add_flag("--skip-auth-check", probe_opts.skip_auth_check,
                       "Skip x509 proxy preflight (local GSI-less rehearsal)");

    auto* report_cmd = app.add_subcommand("report", "Summarize a run from its result files");
    auto* version_cmd = app.add_subcommand("version", "Version info");

    CLI11_PARSE(app, argc, argv);

    if (read_cmd->parsed()) return readgen::RunReadCommand(read_opts);

    if (run_cmd->parsed()) {
        const std::vector<CLI::Option*> legacy_config_opts = {
            endpoint_opt,         filelist_opt,         duration_opt,       rate_opt,
            workers_opt,          pattern_opt,          chunk_opt,          vector_chunks_opt,
            vector_fraction_opt,  file_fraction_opt,    max_bytes_opt,      session_timeout_opt,
            connection_window_opt, connection_retry_opt, request_timeout_opt, seed_opt,
            run_id_opt,           job_id_opt,           results_dir_opt,    snapshot_opt,
            no_results_opt,       pushgateway_opt,      pushgateway_job_opt, pushgateway_keep_opt};

        if (!run_workload.empty()) {
            if (AnyCounted(legacy_config_opts)) {
                std::fprintf(stderr,
                             "error: workload mode rejects legacy run config flags "
                             "(edit the JSON, or omit the workload path)\n");
                return 2;
            }
            readgen::WorkloadRunOptions wl_opts;
            wl_opts.workload_path = run_workload;
            wl_opts.target = run_target;
            wl_opts.dry_run = run_cfg.dry_run;
            wl_opts.skip_auth_check = run_skip_auth;
            return readgen::RunWorkloadCommand(wl_opts);
        }

        if (run_target_opt->count() > 0 || run_skip_auth_opt->count() > 0) {
            std::fprintf(stderr,
                         "error: --target and --skip-auth-check require a workload JSON\n");
            return 2;
        }
        if (endpoint_opt->count() == 0 || filelist_opt->count() == 0) {
            std::fprintf(stderr,
                         "error: pass a workload JSON, or both --endpoint and --filelist\n");
            return 2;
        }

        try {
            run_cfg.duration_s = readgen::ParseDurationString(duration_str);
            run_cfg.chunk_size = static_cast<uint32_t>(readgen::ParseSizeString(chunk_str));
            if (!rate_str.empty()) {
                run_cfg.target_rate_input = rate_str;
                run_cfg.target_rate_bps = readgen::ParseRateString(rate_str);
            }
            if (max_bytes_str == "auto") {
                run_cfg.max_bytes_auto = true;
            } else {
                run_cfg.max_bytes = readgen::ParseSizeString(max_bytes_str);
            }
            run_cfg.pattern = ParsePattern(pattern_str);
            run_cfg.filelist_path = filelist_path;
            run_cfg.files = readgen::LoadFileList(filelist_path);
            run_cfg.snapshot_interval_s = readgen::ParseDurationString(snapshot_str);
            run_cfg.session_timeout_s = readgen::ParseDurationString(session_timeout_str);
            run_cfg.write_results = !no_results;
        } catch (const std::exception& e) {
            std::fprintf(stderr, "error: %s\n", e.what());
            return 2;
        }
        return readgen::RunRunCommand(run_cfg);
    }

    if (validate_cmd->parsed()) {
        const auto result = readgen::ValidateWorkloadFile(workload);
        if (!result.ok) {
            for (const auto& issue : result.issues) {
                if (issue.field.empty()) {
                    std::fprintf(stderr, "error: %s\n", issue.message.c_str());
                } else {
                    std::fprintf(stderr, "%s: %s\n", issue.field.c_str(), issue.message.c_str());
                }
            }
            return 2;
        }
        if (!validate_out.empty()) {
            std::ofstream out(validate_out);
            if (!out) {
                std::fprintf(stderr, "error: cannot write %s\n", validate_out.c_str());
                return 2;
            }
            out << result.canonical_json;
            if (!out) {
                std::fprintf(stderr, "error: failed writing %s\n", validate_out.c_str());
                return 2;
            }
        } else {
            std::fputs(result.canonical_json.c_str(), stdout);
        }
        std::fprintf(stderr, "workload_hash=%s\n", result.workload_hash.c_str());
        return 0;
    }
    if (probe_cmd->parsed()) return readgen::RunProbeCommand(probe_opts);
    if (report_cmd->parsed()) return NotImplemented("report", "coming later");
    if (version_cmd->parsed()) {
        std::printf("xrd-readgen %s (%s, XrdCl %s)\n", READGEN_VERSION, readgen::BuildArch(),
                    XrdVERSION);
        return 0;
    }
    return 0;
}

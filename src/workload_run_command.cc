#include "readgen/workload_run_command.hh"

#include "readgen/auth_check.hh"
#include "readgen/run_command.hh"
#include "readgen/workload_spec.hh"

#include <cstdio>
#include <string>

namespace readgen {

int RunWorkloadCommand(const WorkloadRunOptions& opts) {
    const auto validated = ValidateWorkloadFile(opts.workload_path);
    if (!validated.ok) {
        for (const auto& issue : validated.issues) {
            if (issue.field.empty()) {
                std::fprintf(stderr, "error: %s\n", issue.message.c_str());
            } else {
                std::fprintf(stderr, "%s: %s\n", issue.field.c_str(), issue.message.c_str());
            }
        }
        return 2;
    }

    const WorkloadSpec& wl = validated.resolved;

    const TargetSpec* selected = nullptr;
    if (!opts.target.empty()) {
        for (const auto& t : wl.targets) {
            if (t.name == opts.target) {
                selected = &t;
                break;
            }
        }
        if (!selected) {
            std::fprintf(stderr, "target: unknown target name '%s'\n", opts.target.c_str());
            return 2;
        }
    } else if (wl.targets.size() == 1) {
        selected = &wl.targets[0];
    } else if (wl.targets.empty()) {
        std::fprintf(stderr, "targets: no targets to run\n");
        return 2;
    } else {
        std::fprintf(stderr,
                     "targets: --target required when workload has %zu targets\n",
                     wl.targets.size());
        return 2;
    }

    if (!opts.skip_auth_check) {
        if (wl.auth.mode != "x509") {
            std::fprintf(stderr, "auth.mode: unsupported auth mode for run\n");
            return 2;
        }
        const AuthCheckResult auth = CheckX509Credentials(wl.duration_s);
        if (!auth.ok) {
            for (const auto& issue : auth.issues) {
                std::fprintf(stderr, "%s: %s\n", issue.field.c_str(), issue.message.c_str());
            }
            return 2;
        }
    }

    RunConfig cfg = ToRunConfig(wl, *selected);
    cfg.dry_run = opts.dry_run;
    cfg.workload_hash = validated.workload_hash;
    cfg.workload_resolved_json = validated.canonical_json;
    if (!opts.site_map_path.empty()) {
        cfg.site_map_path = opts.site_map_path;
    }
    cfg.sitename_query = opts.sitename_query;

    std::fprintf(stderr, "workload_hash=%s\n", validated.workload_hash.c_str());

    if (opts.run_hook) {
        return opts.run_hook(cfg);
    }
    return RunRunCommand(cfg);
}

}  // namespace readgen

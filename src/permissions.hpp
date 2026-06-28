#pragma once

#include <set>
#include <string>

namespace lc {

// A persistent allowlist of approved action "rules". When the user answers a
// confirmation with "always", the action's rule is recorded here and saved, so
// matching actions skip the prompt in this and future sessions. Rules are opaque
// strings chosen by the tool layer, e.g. "cmd:git" (run_command by first token)
// or "write" (all file writes).
class PermissionStore {
public:
    // Loads existing rules from `path` (one per line). Missing file => empty.
    explicit PermissionStore(std::string path);

    bool allowed(const std::string& rule) const;
    // Add a rule and persist immediately. No-op if already present.
    void add(const std::string& rule);

    const std::set<std::string>& rules() const { return rules_; }

private:
    void save() const;

    std::string path_;
    std::set<std::string> rules_;
};

}  // namespace lc

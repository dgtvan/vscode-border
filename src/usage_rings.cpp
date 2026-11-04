#include "usage_rings.h"
#include "assistant_profile.h"
#include "profile_config.h"
#include "claude_usage.h"
#include "codex_usage.h"

std::vector<ProjectListHudUsageRing> BuildProfileUsageRings() {
    std::vector<ProjectListHudUsageRing> rings;
    if (!g_config.usageMeterEnabled) return rings;
    auto claude = GetClaudeUsageSnapshots();
    auto codex = GetCodexUsageSnapshots();
    for (const auto& p : g_config.profiles) {
        if (!p.usageEnabled || p.type == ProfileType::None) continue;
        ProjectListHudUsageRing ring;
        ring.profile = ProfileId(p);
        ring.showProfileIcon = p.ringBadge && ShowsProfileIcon(ring.profile);
        ring.hasChipColor = p.hasColor;
        ring.chipColor = p.color;
        ring.stale = true;
        if (p.type == ProfileType::Claude) {
            ClaudeUsageSnapshot snapshot;
            snapshot.profile = ring.profile;
            for (const auto& s : claude) if (SameProfile(s.profile, ring.profile)) { snapshot = s; break; }
            ring.sessionPercent = CurrentUsagePercent(snapshot.session);
            ring.weeklyPercent = CurrentUsagePercent(snapshot.weekly);
            ring.stale = IsClaudeUsageStale(snapshot);
            ring.unavailable = IsClaudeUsageUnavailable(snapshot);
            ring.tooltip = DescribeClaudeUsage(snapshot);
        } else {
            CodexUsageSnapshot snapshot;
            snapshot.profile = ring.profile;
            for (const auto& s : codex) if (SameProfile(s.profile, ring.profile)) { snapshot = s; break; }
            ring.sessionPercent = CurrentCodexUsagePercent(snapshot.primary);
            ring.weeklyPercent = CurrentCodexUsagePercent(snapshot.secondary);
            ring.stale = IsCodexUsageStale(snapshot);
            ring.unavailable = IsCodexUsageUnavailable(snapshot);
            ring.tooltip = DescribeCodexUsage(snapshot);
        }
        rings.push_back(ring);
    }
    return rings;
}

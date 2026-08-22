#include "ai_provider.h"
#include "claude_provider.h"

#include <algorithm>

std::vector<AiProvider*> GetAllAiProviders() {
    static ClaudeProvider claudeProvider;
    static std::vector<AiProvider*> registry = {&claudeProvider};
    return registry;
}

AiProvider* GetAiProvider(const std::string& id) {
    for (AiProvider* p : GetAllAiProviders()) {
        if (id == p->Id()) return p;
    }
    return nullptr;
}

void SyncAllAiProviders(const Config& cfg) {
    for (AiProvider* p : GetAllAiProviders()) {
        bool wantEnabled = cfg.aiIndicatorEnabled &&
            std::find(cfg.aiIndicatorProviders.begin(), cfg.aiIndicatorProviders.end(), p->Id()) !=
                cfg.aiIndicatorProviders.end();
        p->SyncInstallation(wantEnabled);
    }
}

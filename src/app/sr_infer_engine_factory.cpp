#include "app/sr_infer_engine_factory.h"

namespace idiff {

SRInferEngineFactory& SRInferEngineFactory::instance() {
    static SRInferEngineFactory factory;
    return factory;
}

void SRInferEngineFactory::register_engine(
    const std::string& name,
    std::function<std::unique_ptr<SRInferEngine>()> creator) {
    registry_[name] = std::move(creator);
}

std::unique_ptr<SRInferEngine> SRInferEngineFactory::create_engine(
    const std::string& name) const {
    auto it = registry_.find(name);
    if (it != registry_.end()) {
        return it->second();
    }
    return nullptr;
}

bool SRInferEngineFactory::has_engine(const std::string& name) const {
    return registry_.find(name) != registry_.end();
}

std::vector<std::string> SRInferEngineFactory::registered_names() const {
    std::vector<std::string> names;
    names.reserve(registry_.size());
    for (const auto& [name, _] : registry_) {
        names.push_back(name);
    }
    return names;
}

} // namespace idiff
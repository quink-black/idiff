#ifndef IDIFF_SR_INFER_ENGINE_FACTORY_H
#define IDIFF_SR_INFER_ENGINE_FACTORY_H

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

#include "app/sr_infer_engine.h"

namespace idiff {

// Factory for creating and managing SR inference engine instances.
// Supports runtime registration of new engine types so that the UI
// layer never needs to reference concrete engine classes directly.
class SRInferEngineFactory {
public:
    // Get the singleton factory instance.
    static SRInferEngineFactory& instance();

    // Register a concrete engine type under the given name.
    // The creator function must return a heap-allocated SRInferEngine
    // (the factory takes ownership via unique_ptr).
    void register_engine(
        const std::string& name,
        std::function<std::unique_ptr<SRInferEngine>()> creator);

    // Create an engine instance by registered name.
    // Returns nullptr if the name is not registered.
    std::unique_ptr<SRInferEngine> create_engine(
        const std::string& name) const;

    // Check whether an engine type is registered under the given name.
    bool has_engine(const std::string& name) const;

    // Get a list of all registered engine names (for UI display).
    std::vector<std::string> registered_names() const;

private:
    SRInferEngineFactory() = default;

    std::unordered_map<std::string,
        std::function<std::unique_ptr<SRInferEngine>()>> registry_;
};

} // namespace idiff

#endif // IDIFF_SR_INFER_ENGINE_FACTORY_H
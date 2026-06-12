#include "DeletionQueue.hpp"

#include <ranges>
#include "BasicServices/Log.h"
#include "fmt/base.h"

namespace graphics
{
    void DeletionQueue::pushFunction(std::function<void()>&& func, str&& name) {
        deletors.emplace_back(std::move(func));
        names.emplace_back(std::move(name));
    }

    void DeletionQueue::flush() {
        for (auto & deletor : std::ranges::reverse_view(deletors)) {
            auto deletedName = names.back();
            names.pop_back();
            services::Log::Trace(("Deletion queue: " + deletedName).c_str());
            deletor();
        }
        deletors.clear();
        names.clear();
    }
}

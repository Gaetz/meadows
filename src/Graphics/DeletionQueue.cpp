#include "DeletionQueue.hpp"

#include <ranges>
#include "BasicServices/Log.h"
#include "fmt/base.h"

namespace graphics
{
    void DeletionQueue::pushFunction(std::function<void()>&& func, const str& name) {
        deletors.push_back(std::move(func));
        names.push_back(name);
    }

    void DeletionQueue::flush() {
        for (auto & deletor : std::ranges::reverse_view(deletors)) {
            auto deletedName = names.back();
            names.pop_back();
            services::Log::Info(("Deletion queue: " + deletedName).c_str());
            deletor();
        }
        deletors.clear();
        names.clear();
    }
}

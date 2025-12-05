#include "DeletionQueue.hpp"

namespace graphics
{
    void DeletionQueue::pushFunction(std::function<void()>&& func) {
        deletors.push_back(std::move(func));
    }

    void DeletionQueue::flush() {
        for (auto it = deletors.rbegin(); it != deletors.rend(); ++it) {
            (*it)();
        }
        deletors.clear();
    }
}

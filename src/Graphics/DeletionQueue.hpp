#pragma once

#include <deque>
#include <functional>
#include "Defines.h"

namespace  graphics
{
    /*
     * Doing callbacks like this is inefficient at scale, because we are storing
     * whole std::functions for every object we are deleting, which is not going
     * to be optimal. For the amount of objects we will use here,
     * it's going to be fine. But if you need to delete thousands of objects and
     * want them deleted faster, a better implementation would be to store arrays
     * of vulkan handles of various types such as VkImage, VkBuffer, and so on.
     * And then delete those from a loop.
     */
    class DeletionQueue
    {
    public:
        void pushFunction(std::function<void()> &&func, const str& name);
        void flush();
    private:
        std::deque<std::function<void()>> deletors;
        std::deque<str> names;
    };
}


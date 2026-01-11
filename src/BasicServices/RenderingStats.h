#pragma once
#include "Defines.h"

namespace services {
    class RenderingStats {
    public:
        static RenderingStats& Instance() {
            static RenderingStats instance;
            return instance;
        }

        f32 frameTime = 0.0f;
        i32 triangleCount = 0;
        i32 drawcallCount = 0;
        f32 sceneUpdateTime = 0.0f;
        f32 meshDrawTime = 0.0f;

    private:
        RenderingStats() = default;
        ~RenderingStats() = default;

        // Empêcher copie et déplacement
        RenderingStats(const RenderingStats&) = delete;
        RenderingStats& operator=(const RenderingStats&) = delete;
        RenderingStats(RenderingStats&&) = delete;
        RenderingStats& operator=(RenderingStats&&) = delete;
    };
}
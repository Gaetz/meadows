#pragma once
#include "Defines.h"

class Scene {
public:
    Scene() = default;
    ~Scene() = default;

    void update();
    void draw();

    void addNode(str assetName);
};



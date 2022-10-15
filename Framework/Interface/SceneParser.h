#pragma once
#include <memory>
#include <string>
#include "Interface.h"
#include "SceneNode.h"
#include "AssetLoader.h"
#include "Scene.h"

namespace Corona
{
    Interface SceneParser
    {
    public:
        virtual void Parse(std::unique_ptr<Scene>& pScene) = 0;
    };
}
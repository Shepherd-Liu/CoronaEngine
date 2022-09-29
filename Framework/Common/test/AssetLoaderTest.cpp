#include <iostream>
#include <string>
#include "AssetLoader.h"
#include "MemoryManager.h"

using namespace std;
using namespace Corona;

namespace Corona 
{
    MemoryManager* g_pMemoryManager = new MemoryManager();
}

int main(int , char** )
{
    g_pMemoryManager->Initialize();

    AssetLoader asset_loader;
    string shader_pgm = asset_loader.SyncOpenAndReadTextFileToString("Shaders/illum.hs");

    cout << shader_pgm;

    g_pMemoryManager->Finalize();

    delete g_pMemoryManager;

    return 0;
}


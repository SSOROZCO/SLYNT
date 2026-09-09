#pragma once
#include <functional>
#include <vector>
#include <memory>
#include "library_store.h"

class LibraryBridge {
public:
    LibraryBridge(std::shared_ptr<LibraryStore> store);
    void Show(std::function<void(std::vector<TrackDataLite>)> on_selected);
    void Hide();
private:
    struct Impl;
    std::shared_ptr<Impl> impl;
};

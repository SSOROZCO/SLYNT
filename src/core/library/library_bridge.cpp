#include "library_bridge.h"
#include "main.h"

#include <slint.h>

#include <cstdio>
#include <array>
#include <filesystem>
#include <thread>
#include <chrono>
#include <atomic>
#include <memory>
#include <string>
#include <vector>
#include <optional>

static std::string open_folder_dialog() {
    const char* cmds[] = {
        "zenity --file-selection --directory --title='Elegí carpeta de música' 2>/dev/null",
        "kdialog --getexistingdirectory ~/ 2>/dev/null",
        nullptr
    };

    for (int i = 0; cmds[i]; ++i) {
        std::array<char, 1024> buf{};
        std::string out;

        FILE* f = popen(cmds[i], "r");
        if (!f)
            continue;

        while (fgets(buf.data(), buf.size(), f))
            out += buf.data();

        pclose(f);

        while (!out.empty() &&
               (out.back() == '\n' || out.back() == '\r'))
            out.pop_back();

        if (!out.empty())
            return out;
    }

    return "";
}


// ============================================================
// IMPLEMENTACIÓN PRIVADA
// ============================================================

struct LibraryBridge::Impl {

    std::optional<slint::ComponentHandle<LibraryWindow>> window;

    std::function<void(std::vector<TrackDataLite>)> on_selected;

    std::shared_ptr<LibraryStore> store;

    std::string last_folder;

    std::atomic<bool> isScanning{false};
    std::atomic<bool> isRefreshing{false};

    std::shared_ptr<std::atomic<uint64_t>> search_generation =
        std::make_shared<std::atomic<uint64_t>>(0);
};


// ============================================================
// CONSTRUCTOR
// ============================================================

LibraryBridge::LibraryBridge(std::shared_ptr<LibraryStore> store)
    : impl(std::make_shared<Impl>())
{
    impl->store = store;
}


// ============================================================
// SHOW
// ============================================================

void LibraryBridge::Show(
    std::function<void(std::vector<TrackDataLite>)> on_selected)
{
    impl->on_selected = on_selected;

    // --------------------------------------------------------
    // Si la ventana ya existe, mostrarla.
    // --------------------------------------------------------

    if (impl->window.has_value()) {

        auto w = impl->window;

        slint::invoke_from_event_loop(
            [w]() mutable {

                if (w.has_value())
                    w.value()->show();
            });

        return;
    }


    // ========================================================
    // CREAR VENTANA
    // ========================================================

    auto win = LibraryWindow::create();


    // ========================================================
    // FUNCIÓN PARA CONSTRUIR EL MODELO VISUAL
    //
    // Esta parte corre en el hilo de Slint porque modifica
    // propiedades y modelos de LibraryWindow.
    // ========================================================

    auto applyAlbums =
        [win, this](std::vector<AlbumData> data,
                    std::string filter)
        {

            auto model =
                std::make_shared<
                    slint::VectorModel<Album>>();


            for (auto& a : data) {

                Album sl;

                sl.id = a.id;

                sl.title =
                    slint::SharedString(a.title);

                sl.artist =
                    slint::SharedString(a.artist);

                sl.year =
                    slint::SharedString(a.year);


                // ------------------------------------------------
                // PORTADA
                // ------------------------------------------------

                sl.has_cover =
                    !a.cover_path.empty();


                if (!a.cover_path.empty() &&
                    std::filesystem::exists(a.cover_path)) {

                    std::string thumbPath =
                        a.cover_path;


                    if (thumbPath.find(
                            "/.cache/slynt/thumbs/")
                        == std::string::npos) {

                        thumbPath =
                            impl->store->
                            GetCachedThumbnail(
                                a.cover_path);
                    }


                    if (!thumbPath.empty()) {

                        sl.cover =
                            slint::Image::load_from_path(
                                slint::SharedString(
                                    thumbPath));
                    }
                }


                sl.root_path =
                    slint::SharedString(
                        a.root_path);

                sl.track_count =
                    a.track_count;


                model->push_back(sl);
            }


            win->set_albums(model);

            win->set_found_albums(
                static_cast<int>(
                    data.size()));


            if (data.empty() &&
                filter.empty()) {

                win->set_state(
                    slint::SharedString(
                        "empty"));

            } else {

                win->set_state(
                    slint::SharedString(
                        "list"));
            }
        };


    // ========================================================
    // REFRESH ASÍNCRONO
    //
    // Toda consulta SQLite ocurre fuera del hilo de Slint.
    // ========================================================

    auto refreshAsync =
        [win, this, applyAlbums]()
        {

            if (impl->isRefreshing.exchange(true))
                return;


            auto store =
                impl->store;


            slint::invoke_from_event_loop(
                [win]
                {

                    win->set_library_busy(true);

                    win->set_library_status(
                        slint::SharedString(
                            "Actualizando…"));
                });


            std::thread(
                [win,
                 this,
                 store,
                 applyAlbums]()
                {

                    auto data =
                        store->GetAlbums();


                    slint::invoke_from_event_loop(
                        [win,
                         this,
                         data = std::move(data),
                         applyAlbums]
                        () mutable
                        {

                            // ------------------------------------
                            // Actualizar interfaz.
                            // ------------------------------------

                            applyAlbums(
                                std::move(data),
                                "");


                            win->set_library_busy(
                                false);

                            win->set_library_status(
                                slint::SharedString(
                                    ""));

                            impl->isRefreshing =
                                false;
                        });

                })
                .detach();
        };


    // ========================================================
    // PRIMERA CARGA
    // ========================================================

    {
        auto store =
            impl->store;

        std::thread(
            [win, store, applyAlbums]()
            {

                auto data =
                    store->GetAlbums();


                slint::invoke_from_event_loop(
                    [win,
                     data = std::move(data),
                     applyAlbums]
                    () mutable
                    {

                        applyAlbums(
                            std::move(data),
                            "");

                    });

            })
            .detach();
    }


    // ========================================================
    // AGREGAR CARPETA
    // ========================================================

    win->on_add_folder(
        [win, this]()
        {

            if (impl->isScanning.load())
                return;


            std::string folder =
                open_folder_dialog();


            if (folder.empty())
                return;


            impl->last_folder =
                folder;

            impl->isScanning =
                true;


            slint::invoke_from_event_loop(
                [win, folder]
                {

                    win->set_state(
                        slint::SharedString(
                            "loading"));

                    win->set_percent(0);

                    win->set_current_folder(
                        slint::SharedString(
                            folder));

                    win->set_found_albums(0);
                });


            auto store =
                impl->store;


            store->ScanFolderAsync(

                folder,

                [win](
                    int pct,
                    std::string cur,
                    int found)
                {

                    slint::invoke_from_event_loop(
                        [win,
                         pct,
                         cur = std::move(cur),
                         found]
                        {

                            win->set_percent(
                                pct);

                            win->set_current_folder(
                                slint::SharedString(
                                    cur));

                            win->set_found_albums(
                                found);
                        });
                },

                [win, this]()
                {

                    impl->isScanning =
                        false;


                    slint::invoke_from_event_loop(
                        [win, this]
                        {

                            auto store =
                                impl->store;


                            std::thread(
                                [win, store]()
                                {

                                    auto data =
                                        store->GetAlbums();


                                    slint::invoke_from_event_loop(
                                        [win,
                                         data = std::move(data)]
                                        () mutable
                                        {

                                            auto model =
                                                std::make_shared<
                                                    slint::VectorModel<Album>>();


                                            for (auto& a : data) {

                                                Album sl;

                                                sl.id =
                                                    a.id;

                                                sl.title =
                                                    slint::SharedString(
                                                        a.title);

                                                sl.artist =
                                                    slint::SharedString(
                                                        a.artist);

                                                sl.year =
                                                    slint::SharedString(
                                                        a.year);

                                                sl.has_cover =
                                                    !a.cover_path.empty();


                                                if (!a.cover_path.empty() &&
                                                    std::filesystem::exists(
                                                        a.cover_path)) {

                                                    std::string thumb =
                                                        a.cover_path;


                                                    if (thumb.find(
                                                            "/.cache/slynt/thumbs/")
                                                        == std::string::npos) {

                                                        // Se genera solamente
                                                        // cuando hace falta.
                                                        //
                                                        // Esto queda fuera de
                                                        // este bloque de UI en
                                                        // la siguiente etapa.
                                                    }


                                                    if (!thumb.empty()) {

                                                        sl.cover =
                                                            slint::Image::
                                                            load_from_path(
                                                                slint::SharedString(
                                                                    thumb));
                                                    }
                                                }


                                                sl.root_path =
                                                    slint::SharedString(
                                                        a.root_path);

                                                sl.track_count =
                                                    a.track_count;


                                                model->push_back(sl);
                                            }


                                            win->set_albums(
                                                model);

                                            win->set_found_albums(
                                                static_cast<int>(
                                                    data.size()));

                                            win->set_state(
                                                slint::SharedString(
                                                    data.empty()
                                                        ? "empty"
                                                        : "list"));
                                        });

                                })
                                .detach();

                        });
                }
            );
        });


    // ========================================================
    // ACTUALIZAR BIBLIOTECA
    // ========================================================

    win->on_refresh_library(
        [win, this, refreshAsync]()
        {

            if (impl->isScanning.load())
                return;


            refreshAsync();
        });


    // ========================================================
    // CONFIRMAR BORRADO
    // ========================================================

    win->on_confirm_yes(
        [win, this]()
        {

            // -----------------------------------------------
            // Capturamos el tipo e ID ANTES de lanzar el
            // worker.
            // -----------------------------------------------

            std::string type =
                std::string(
                    win->get_confirm_type().data());


            int albumId =
                win->get_confirm_album_id();


            if (type.empty())
                return;


            auto store =
                impl->store;


            // -----------------------------------------------
            // Cerramos inmediatamente el diálogo.
            // -----------------------------------------------

            win->set_confirm_type(
                slint::SharedString(""));

            win->set_confirm_album_id(
                -1);


            // -----------------------------------------------
            // Mostrar actividad.
            // -----------------------------------------------

            win->set_library_busy(true);

            win->set_library_status(
                slint::SharedString(
                    "Actualizando…"));


            // -----------------------------------------------
            // SQLite fuera del hilo de UI.
            // -----------------------------------------------

            std::thread(
                [win,
                 this,
                 store,
                 type,
                 albumId]()
                {

                    if (type == "all") {

                        store->DeleteAll();

                    } else if (type == "album") {

                        store->DeleteAlbum(
                            albumId);
                    }


                    auto data =
                        store->GetAlbums();


                    slint::invoke_from_event_loop(
                        [win,
                         this,
                         data = std::move(data)]
                        () mutable
                        {

                            auto model =
                                std::make_shared<
                                    slint::VectorModel<Album>>();


                            for (auto& a : data) {

                                Album sl;

                                sl.id =
                                    a.id;

                                sl.title =
                                    slint::SharedString(
                                        a.title);

                                sl.artist =
                                    slint::SharedString(
                                        a.artist);

                                sl.year =
                                    slint::SharedString(
                                        a.year);

                                sl.has_cover =
                                    !a.cover_path.empty();


                                if (!a.cover_path.empty() &&
                                    std::filesystem::exists(
                                        a.cover_path)) {

                                    sl.cover =
                                        slint::Image::
                                        load_from_path(
                                            slint::SharedString(
                                                a.cover_path));
                                }


                                sl.root_path =
                                    slint::SharedString(
                                        a.root_path);

                                sl.track_count =
                                    a.track_count;


                                model->push_back(sl);
                            }


                            win->set_albums(
                                model);

                            win->set_found_albums(
                                static_cast<int>(
                                    data.size()));


                            win->set_state(
                                slint::SharedString(
                                    data.empty()
                                        ? "empty"
                                        : "list"));


                            win->set_library_busy(
                                false);

                            win->set_library_status(
                                slint::SharedString(
                                    ""));

                            impl->isRefreshing =
                                false;
                        });

                })
                .detach();
        });


    // ========================================================
    // CANCELAR BORRADO
    // ========================================================

    win->on_confirm_no(
        [win]()
        {

            win->set_confirm_type(
                slint::SharedString(""));

            win->set_confirm_album_id(
                -1);
        });


    // ========================================================
    // REINTENTAR ESCANEO
    // ========================================================

    win->on_retry(
        [win, this]()
        {

            if (impl->last_folder.empty() ||
                impl->isScanning.load())
                return;


            std::string folder =
                impl->last_folder;


            impl->isScanning =
                true;


            slint::invoke_from_event_loop(
                [win]
                {

                    win->set_state(
                        slint::SharedString(
                            "loading"));

                    win->set_percent(0);
                });


            auto store =
                impl->store;


            store->ScanFolderAsync(

                folder,

                [win](
                    int pct,
                    std::string cur,
                    int found)
                {

                    slint::invoke_from_event_loop(
                        [win,
                         pct,
                         cur = std::move(cur),
                         found]
                        {

                            win->set_percent(
                                pct);

                            win->set_current_folder(
                                slint::SharedString(
                                    cur));

                            win->set_found_albums(
                                found);
                        });
                },

                [win, this]()
                {

                    impl->isScanning =
                        false;


                    slint::invoke_from_event_loop(
                        [win, this]
                        {

                            auto store =
                                impl->store;


                            std::thread(
                                [win, store]()
                                {

                                    auto data =
                                        store->GetAlbums();


                                    slint::invoke_from_event_loop(
                                        [win,
                                         data = std::move(data)]
                                        () mutable
                                        {

                                            auto model =
                                                std::make_shared<
                                                    slint::VectorModel<Album>>();


                                            for (auto& a : data) {

                                                Album sl;

                                                sl.id =
                                                    a.id;

                                                sl.title =
                                                    slint::SharedString(
                                                        a.title);

                                                sl.artist =
                                                    slint::SharedString(
                                                        a.artist);

                                                sl.year =
                                                    slint::SharedString(
                                                        a.year);

                                                sl.has_cover =
                                                    !a.cover_path.empty();

                                                sl.root_path =
                                                    slint::SharedString(
                                                        a.root_path);

                                                sl.track_count =
                                                    a.track_count;


                                                model->push_back(
                                                    sl);
                                            }


                                            win->set_albums(
                                                model);

                                            win->set_found_albums(
                                                static_cast<int>(
                                                    data.size()));

                                            win->set_state(
                                                slint::SharedString(
                                                    data.empty()
                                                        ? "empty"
                                                        : "list"));
                                        });

                                })
                                .detach();

                        });
                }
            );
        });


    // ========================================================
    // DOBLE CLICK SOBRE ÁLBUM
    // ========================================================

    win->on_album_double_clicked(
        [this](int id)
        {

            auto tracks =
                impl->store->
                GetTracksForAlbum(id);


            auto cb =
                impl->on_selected;


            auto implCopy =
                impl;


            slint::invoke_from_event_loop(
                [implCopy,
                 cb,
                 tracks = std::move(tracks)]
                () mutable
                {

                    if (cb)
                        cb(tracks);


                    if (implCopy->window.has_value())
                        implCopy->window.value()->hide();
                });
        });


    // ========================================================
    // CERRAR
    // ========================================================

    win->on_close(
        [this]
        {

            auto w =
                impl->window;


            slint::invoke_from_event_loop(
                [w]() mutable
                {

                    if (w.has_value())
                        w.value()->hide();
                });
        });


    // ========================================================
    // REMOVE FOLDER
    // ========================================================

    win->on_remove_folder(
        [](slint::SharedString)
        {
            // Reservado para futura implementación.
        });


    // ========================================================
    // MOSTRAR
    // ========================================================

    win->show();

    impl->window =
        win;
}


// ============================================================
// HIDE
// ============================================================

void LibraryBridge::Hide()
{
    auto w =
        impl->window;


    slint::invoke_from_event_loop(
        [w]() mutable
        {

            if (w.has_value())
                w.value()->hide();
        });
}


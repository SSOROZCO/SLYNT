#include "main.h"
#include "player.h"

#include <gst/gst.h>

#include <filesystem>
#include <thread>
#include <vector>
#include <algorithm>
#include <random>
#include <cstdio>
#include <iostream>
#include <fstream>
#include <sstream>
#include <regex>
#include <set>
#include <string>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <chrono>
#include <optional>
#include <cstdlib>

#include <taglib/fileref.h>
#include <taglib/tag.h>

#include "core/library/library_store.h"
#include "core/library/library_bridge.h"



// ============================================================================
// TRACK DATA
// ============================================================================

struct TrackData {
    std::string path;
    std::string title;
    std::string artist;
    std::string album;
    std::string duration = "--:--";
    std::string search_blob;
    std::string year;

    int id = -1;

    int sampleRate = 44100;
    int bits = 16;
    int chans = 2;

    bool is_dsd = false;
};


// ============================================================================
// DAC
// ============================================================================

struct DacInfo {
    std::string name = "No DAC";
    std::string device = "";
    std::string backend = "---";
};


DacInfo detectDac()
{
    DacInfo info;

    std::ifstream f("/proc/asound/cards");

    if (!f.is_open())
        return info;

    std::string line;

    while (std::getline(f, line)) {

        if (line.empty() || line[0] != ' ')
            continue;

        std::istringstream iss(line);

        int id;

        if (!(iss >> id))
            continue;

        auto dashPos = line.find(" - ");

        if (dashPos == std::string::npos)
            continue;

        std::string rawName = line.substr(dashPos + 3);

        rawName.erase(
            0,
            rawName.find_first_not_of(" \t")
        );

        rawName.erase(
            rawName.find_last_not_of(" \t\r\n") + 1
        );

        if (line.find("USB-Audio") != std::string::npos) {

            info.name =
                rawName.empty()
                    ? "USB Audio Device"
                    : rawName;

            info.device =
                "hw:" + std::to_string(id) + ",0";

            info.backend = "ALSA Direct";

            return info;
        }
    }

    return info;
}


// ============================================================================
// LIVE HARDWARE
// ============================================================================

struct LiveHw {
    std::string rate = "—";
    std::string format = "—";
    std::string access = "—";

    bool active = false;

    int rateHz = 0;
};


LiveHw getLiveHwParams(const std::string& device)
{
    LiveHw live;

    if (device.empty())
        return live;

    int cardId = 0;

    std::regex re(R"(hw:(\d+))");
    std::smatch m;

    if (std::regex_search(device, m, re))
        cardId = std::stoi(m[1]);
    else
        return live;

    std::string path =
        "/proc/asound/card"
        + std::to_string(cardId)
        + "/pcm0p/sub0/hw_params";

    std::ifstream f(path);

    if (!f.is_open())
        return live;

    std::string line;

    while (std::getline(f, line)) {

        if (line.find("closed") != std::string::npos) {

            live.active = false;

            return live;
        }

        if (line.rfind("format:", 0) == 0) {

            live.format = line.substr(7);

            live.format.erase(
                0,
                live.format.find_first_not_of(" \t")
            );

            live.format.erase(
                live.format.find_last_not_of(" \t\r\n") + 1
            );
        }

        if (line.rfind("rate:", 0) == 0) {

            auto start =
                line.find_first_of("0123456789");

            if (start != std::string::npos) {

                auto end =
                    line.find_first_not_of(
                        "0123456789",
                        start
                    );

                int rate =
                    std::stoi(
                        line.substr(
                            start,
                            end - start
                        )
                    );

                live.rateHz = rate;

                char buf[32];

                if (rate >= 1000) {

                    snprintf(
                        buf,
                        sizeof(buf),
                        "%.1f kHz",
                        rate / 1000.0
                    );

                } else {

                    snprintf(
                        buf,
                        sizeof(buf),
                        "%d Hz",
                        rate
                    );
                }

                live.rate = buf;

                live.active = true;
            }
        }

        if (line.rfind("access:", 0) == 0) {

            live.access = line.substr(7);

            live.access.erase(
                0,
                live.access.find_first_not_of(" \t")
            );

            live.access.erase(
                live.access.find_last_not_of(" \t\r\n") + 1
            );
        }
    }

    return live;
}


// ============================================================================
// HELPERS
// ============================================================================

inline bool isDsdPath(const std::string& p)
{
    std::string lp = p;

    std::transform(
        lp.begin(),
        lp.end(),
        lp.begin(),
        ::tolower
    );

    return
        lp.find(".dsf") != std::string::npos
        ||
        lp.find(".dff") != std::string::npos;
}


inline std::string toLower(std::string s)
{
    std::transform(
        s.begin(),
        s.end(),
        s.begin(),
        ::tolower
    );

    return s;
}


// ============================================================================
// SEARCH ENGINE
//
// Un solo worker.
//
// La UI solamente entrega el texto.
// El worker espera 150 ms.
// Si llega otra letra durante ese tiempo,
// reemplaza la búsqueda pendiente.
//
// La búsqueda pesada NO ocurre en el hilo de Slint.
// ============================================================================

class SearchEngine {
public:

    using TrackSnapshot =
        std::shared_ptr<const std::vector<TrackData>>;


    SearchEngine(
        slint::ComponentHandle<MainWindow> window,
        std::shared_ptr<
            slint::VectorModel<Track>
        > originalModel,
        std::shared_ptr<
            std::atomic<std::shared_ptr<const std::vector<TrackData>>>
        > index
    )
        : window_(window),
          originalModel_(originalModel),
          index_(index)
    {
        worker_ =
            std::thread(
                [this] {
                    workerLoop();
                }
            );
    }


    ~SearchEngine()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);

            stop_ = true;
        }

        condition_.notify_one();

        if (worker_.joinable())
            worker_.join();
    }


    void request(const std::string& query)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);

            pendingQuery_ = query;

            ++generation_;

            pendingGeneration_ = generation_;
        }

        condition_.notify_one();
    }


private:

    slint::ComponentHandle<MainWindow> window_;

    std::shared_ptr<
        slint::VectorModel<Track>
    > originalModel_;

    std::shared_ptr<
        std::atomic<
            std::shared_ptr<
                const std::vector<TrackData>
            >
        >
    > index_;


    std::thread worker_;

    std::mutex mutex_;

    std::condition_variable condition_;

    bool stop_ = false;

    std::string pendingQuery_;

    uint64_t generation_ = 0;

    uint64_t pendingGeneration_ = 0;


    void workerLoop()
    {
        while (true) {

            std::string query;

            uint64_t myGeneration = 0;


            // --------------------------------------------------------
            // ESPERAR UNA PETICIÓN
            // --------------------------------------------------------

            {
                std::unique_lock<std::mutex> lock(mutex_);

                condition_.wait(
                    lock,
                    [this] {
                        return stop_
                            || !pendingQuery_.empty()
                            || pendingGeneration_ != 0;
                    }
                );

                if (stop_)
                    return;

                query = pendingQuery_;

                myGeneration =
                    pendingGeneration_;
            }


            // --------------------------------------------------------
            // DEBOUNCE
            //
            // Esperamos 150 ms.
            //
            // Si el usuario sigue escribiendo,
            // no buscamos todavía.
            // --------------------------------------------------------

            std::this_thread::sleep_for(
                std::chrono::milliseconds(150)
            );


            // --------------------------------------------------------
            // ¿LLEGÓ OTRA BÚSQUEDA?
            // --------------------------------------------------------

            {
                std::lock_guard<std::mutex> lock(mutex_);

                if (stop_)
                    return;

                if (myGeneration != generation_)
                    continue;
            }


            // --------------------------------------------------------
            // NORMALIZAR
            // --------------------------------------------------------

            std::string q = toLower(query);


            // --------------------------------------------------------
            // SNAPSHOT DEL ÍNDICE
            //
            // No tocamos allTracks directamente.
            // --------------------------------------------------------

            TrackSnapshot snapshot =
                std::atomic_load(index_.get());


            if (!snapshot) {

                slint::invoke_from_event_loop(
                    [this, myGeneration] {

                        std::lock_guard<std::mutex> lock(mutex_);

                        if (myGeneration != generation_)
                            return;

                        window_->set_filtered(
                            originalModel_
                        );
                    }
                );

                continue;
            }


            // --------------------------------------------------------
            // BÚSQUEDA VACÍA
            // --------------------------------------------------------

            if (q.empty()) {

                slint::invoke_from_event_loop(
                    [this, myGeneration] {

                        std::lock_guard<std::mutex> lock(mutex_);

                        if (myGeneration != generation_)
                            return;

                        window_->set_filtered(
                            originalModel_
                        );
                    }
                );

                continue;
            }


            // --------------------------------------------------------
            // CREAR RESULTADOS
            // --------------------------------------------------------

            auto filtered =
                std::make_shared<
                    slint::VectorModel<Track>
                >();


            for (const auto& td : *snapshot) {

                // -----------------------------------------------
                // Si llegó una búsqueda nueva,
                // abortamos cuanto antes.
                // -----------------------------------------------

                {
                    std::lock_guard<std::mutex> lock(mutex_);

                    if (myGeneration != generation_)
                        break;
                }


                if (td.search_blob.find(q)
                    == std::string::npos)
                    continue;


                Track t;

                t.id = td.id;

                t.title =
                    slint::SharedString(td.title);

                t.path =
                    slint::SharedString(td.path);

                t.codec =
                    slint::SharedString(
                        td.is_dsd
                            ? "DSF"
                            : "FLAC"
                    );

                t.duration =
                    slint::SharedString(td.duration);

                t.artist =
                    slint::SharedString(td.artist);

                t.album =
                    slint::SharedString(td.album);

                t.year =
                    slint::SharedString(td.year);


                filtered->push_back(t);
            }


            // --------------------------------------------------------
            // PUBLICAR RESULTADO EN SLINT
            // --------------------------------------------------------

            slint::invoke_from_event_loop(
                [
                    this,
                    filtered,
                    myGeneration
                ] {

                    std::lock_guard<std::mutex> lock(mutex_);

                    // Resultado viejo:
                    // no tocar la UI.
                    if (myGeneration != generation_)
                        return;

                    window_->set_filtered(
                        filtered
                    );
                }
            );
        }
    }
};


// ============================================================================
// MAIN
// ============================================================================

int main(int argc, char** argv)
{
    gst_init(&argc, &argv);


    auto window =
        MainWindow::create();


    auto player =
        std::make_shared<SimplePlayer>();


    player->setOutputMode(
        OutputMode::Exclusive
    );


    // ========================================================================
    // DATOS PRINCIPALES
    // ========================================================================

    auto allTracks =
        std::make_shared<
            std::vector<TrackData>
        >();


    auto shuffleEnabled =
        std::make_shared<bool>(false);


    auto repeatMode =
        std::make_shared<std::string>("Off");


    auto shuffledOrder =
        std::make_shared<
            std::vector<int>
        >();


    auto shuffledPos =
        std::make_shared<int>(0);


    // ========================================================================
    // MODELO ORIGINAL
    // ========================================================================

    auto original_model =
        std::make_shared<
            slint::VectorModel<Track>
        >();


    // ========================================================================
    // ÍNDICE INMUTABLE PARA BÚSQUEDA
    //
    // El buscador nunca toca allTracks directamente.
    //
    // Esto evita que la búsqueda lea un vector mientras otra operación
    // lo está modificando.
    // ========================================================================

    auto searchIndex =
        std::make_shared<
            std::atomic<
                std::shared_ptr<
                    const std::vector<TrackData>
                >
            >
        >(
            std::make_shared<
                const std::vector<TrackData>
            >()
        );


    // ========================================================================
    // LIBRARY
    // ========================================================================

    auto libraryStore =
        std::make_shared<LibraryStore>();


    libraryStore->Open();


    auto libraryBridge =
        std::make_shared<
            LibraryBridge
        >(libraryStore);


    // ========================================================================
    // SPECTRUM
    // ========================================================================

    player->onSpectrum =
        [window](std::vector<float> bars) {

            slint::invoke_from_event_loop(
                [window, bars] {

                    auto model =
                        std::make_shared<
                            slint::VectorModel<float>
                        >();


                    for (float v : bars)
                        model->push_back(v);


                    window->set__spectrum(
                        model
                    );
                }
            );
        };


    // ========================================================================
    // COVER
    // ========================================================================

    auto getCover =
        [](std::filesystem::path folder)
            -> std::string
    {
        const std::vector<std::string> priority = {
            "cover.jpg",
            "Cover.jpg",
            "COVER.JPG",
            "folder.jpg",
            "Folder.jpg",
            "FOLDER.JPG",
            "front.jpg",
            "Front.jpg",
            "FRONT.JPG",
            "cover.png",
            "Cover.png",
            "folder.png"
        };


        for (auto& n : priority) {

            auto p = folder / n;

            if (std::filesystem::exists(p))
                return p.string();
        }


        try {

            for (auto& e :
                 std::filesystem::directory_iterator(folder)) {

                if (!e.is_regular_file())
                    continue;


                auto ext =
                    e.path()
                      .extension()
                      .string();


                std::transform(
                    ext.begin(),
                    ext.end(),
                    ext.begin(),
                    ::tolower
                );


                if (
                    ext == ".jpg"
                    ||
                    ext == ".jpeg"
                    ||
                    ext == ".png"
                    ||
                    ext == ".webp"
                ) {

                    return e.path().string();
                }
            }

        } catch (...) {
        }


        return "";
    };


    // ========================================================================
    // ESTADO INICIAL
    // ========================================================================

    slint::invoke_from_event_loop(
        [window] {

            auto dac =
                detectDac();


            window->set__dac(
                slint::SharedString(
                    dac.name.empty()
                        ? "No DAC"
                        : dac.name
                )
            );


            window->set__backend(
                slint::SharedString(
                    dac.backend
                )
            );


            window->set__device(
                slint::SharedString(
                    dac.device.empty()
                        ? "Connect the USB DAC"
                        : dac.device + " • idle"
                )
            );


            window->set__signal_path(
                slint::SharedString("—")
            );


            window->set__codec(
                slint::SharedString("—")
            );


            window->set__samplerate(
                slint::SharedString("— kHz")
            );


            window->set__bitdepth(
                slint::SharedString("—-bit")
            );


            window->set__channels(
                slint::SharedString("— ch")
            );


            window->set__bit_perfect(false);


            window->set__title(
                slint::SharedString(
                    "No music loaded"
                )
            );


            window->set__artist(
                slint::SharedString(
                    "Open a folder to get started"
                )
            );


            window->set__album(
                slint::SharedString("—")
            );


            window->set__time_pos(
                slint::SharedString("0:00")
            );


            window->set__time_total(
                slint::SharedString("0:00")
            );


            window->set__is_playing(false);

            window->set__current_id(-1);

            window->set_output_mode(
                slint::SharedString(
                    "exclusive"
                )
            );

            window->set__progress(0);

            window->set_shuffle(false);

            window->set_repeat_mode(
                slint::SharedString("Off")
            );


            auto idle =
                std::make_shared<
                    slint::VectorModel<float>
                >();


            for (int i = 0; i < 32; i++)
                idle->push_back(0.0f);


            window->set__spectrum(
                idle
            );
        }
    );


    // ========================================================================
    // UI HELPERS
    // ========================================================================

    auto setDsdUi =
        [](auto window,
           auto& td,
           auto player,
           const std::string& dacName)
    {
        window->set__codec(
            slint::SharedString("DSD")
        );


        window->set__samplerate(
            slint::SharedString(
                "2.8 MHz (DSD64)"
            )
        );


        window->set__bitdepth(
            slint::SharedString("1-bit")
        );


        window->set__channels(
            slint::SharedString(
                std::to_string(td.chans)
                + " ch"
            )
        );


        window->set__signal_path(
            slint::SharedString(
                "DSD -> DoP -> "
                + dacName
            )
        );


        window->set__bit_perfect(true);
    };


    auto setPcmUi =
        [](auto window,
           auto& td,
           const std::string& dacName,
           auto player)
    {
        char sr[32];


        snprintf(
            sr,
            sizeof(sr),
            "%.1f kHz",
            td.sampleRate / 1000.0
        );


        window->set__samplerate(
            slint::SharedString(sr)
        );


        window->set__bitdepth(
            slint::SharedString(
                std::to_string(td.bits)
                + "-bit"
            )
        );


        window->set__channels(
            slint::SharedString(
                std::to_string(td.chans)
                + " ch"
            )
        );


        window->set__codec(
            slint::SharedString("FLAC")
        );


        window->set__signal_path(
            slint::SharedString(
                player->isBitPerfectFor(td.path)
                    ? "Direct -> " + dacName
                    : "Convert"
            )
        );


        window->set__bit_perfect(
            player->isBitPerfectFor(
                td.path
            )
        );
    };


    // ========================================================================
    // PLAY AT ID
    // ========================================================================

    auto playAtId =
        [
            player,
            window,
            allTracks,
            getCover,
            setDsdUi,
            setPcmUi
        ](int trackId)
    {
        if (allTracks->empty())
            return;


        if (detectDac().device.empty())
            return;


        auto it =
            std::find_if(
                allTracks->begin(),
                allTracks->end(),
                [trackId](auto& t) {
                    return t.id == trackId;
                }
            );


        if (it == allTracks->end())
            return;


        auto td = *it;


        player->load(td.path);

        player->play();


        std::string coverPath =
            getCover(
                std::filesystem::path(
                    td.path
                ).parent_path()
            );


        slint::invoke_from_event_loop(
            [
                window,
                td,
                player,
                coverPath,
                setDsdUi,
                setPcmUi
            ] {

                window->set__current_id(
                    td.id
                );


                window->set__title(
                    slint::SharedString(
                        td.title
                    )
                );


                window->set__artist(
                    slint::SharedString(
                        td.artist
                    )
                );


                window->set__album(
                    slint::SharedString(
                        td.album
                    )
                );


                window->set__is_playing(
                    true
                );


                auto dac =
                    detectDac();


                window->set__dac(
                    slint::SharedString(
                        dac.name
                    )
                );


                window->set__device(
                    slint::SharedString(
                        player->hwDevice
                    )
                );


                window->set__backend(
                    slint::SharedString(
                        player->hwDevice.rfind(
                            "hw:",
                            0
                        ) == 0
                            ? "ALSA Direct"
                            : "System Audio"
                    )
                );


                if (isDsdPath(td.path))
                    setDsdUi(
                        window,
                        td,
                        player,
                        dac.name
                    );
                else
                    setPcmUi(
                        window,
                        td,
                        dac.name,
                        player
                    );


                if (!coverPath.empty()) {

                    window->set__cover(
                        slint::Image::load_from_path(
                            slint::SharedString(
                                coverPath
                            )
                        )
                    );

                } else {

                    window->set__cover(
                        slint::Image()
                    );
                }
            }
        );
    };


    // ========================================================================
    // NEXT
    // ========================================================================

    auto playNextFn =
        [
            player,
            window,
            allTracks,
            getCover,
            setDsdUi,
            setPcmUi,
            shuffleEnabled,
            repeatMode,
            shuffledOrder,
            shuffledPos,
            playAtId
        ]
    {
        if (allTracks->empty())
            return;


        if (detectDac().device.empty())
            return;


        if (*repeatMode == "One") {

            player->seek(0.0);

            player->play();


            slint::invoke_from_event_loop(
                [window] {
                    window->set__is_playing(
                        true
                    );
                }
            );


            return;
        }


        if (
            *shuffleEnabled
            &&
            !shuffledOrder->empty()
        ) {

            *shuffledPos =
                (
                    *shuffledPos + 1
                )
                %
                (int)shuffledOrder->size();


            int nextId =
                (*allTracks)[
                    (*shuffledOrder)[
                        *shuffledPos
                    ]
                ].id;


            playAtId(nextId);

            return;
        }


        int cur =
            window->get__current_id();


        int pos = -1;


        for (
            int i = 0;
            i < (int)allTracks->size();
            i++
        ) {

            if (
                (*allTracks)[i].id
                ==
                cur
            ) {

                pos = i;

                break;
            }
        }


        if (pos == -1)
            pos = 0;
        else
            pos++;


        if (
            pos >=
            (int)allTracks->size()
        ) {

            if (*repeatMode == "All")
                pos = 0;

            else {

                slint::invoke_from_event_loop(
                    [window] {
                        window->set__is_playing(
                            false
                        );
                    }
                );

                return;
            }
        }


        playAtId(
            (*allTracks)[pos].id
        );
    };


    // ========================================================================
    // PREVIOUS
    // ========================================================================

    auto playPrevFn =
        [
            player,
            window,
            allTracks,
            getCover,
            setDsdUi,
            setPcmUi,
            shuffleEnabled,
            shuffledOrder,
            shuffledPos,
            playAtId
        ]
    {
        if (allTracks->empty())
            return;


        if (detectDac().device.empty())
            return;


        if (
            *shuffleEnabled
            &&
            !shuffledOrder->empty()
        ) {

            *shuffledPos =
                (
                    *shuffledPos
                    - 1
                    +
                    (int)shuffledOrder->size()
                )
                %
                (int)shuffledOrder->size();


            int prevId =
                (*allTracks)[
                    (*shuffledOrder)[
                        *shuffledPos
                    ]
                ].id;


            playAtId(prevId);

            return;
        }


        auto cur =
            window->get__current_id();


        int pos = 0;


        for (
            int i = 0;
            i < (int)allTracks->size();
            i++
        ) {

            if (
                (*allTracks)[i].id
                ==
                cur
            ) {

                pos = i;

                break;
            }
        }


        pos =
            (
                pos - 1
                +
                (int)allTracks->size()
            )
            %
            (int)allTracks->size();


        playAtId(
            (*allTracks)[pos].id
        );
    };


    player->onTrackFinished =
        playNextFn;


    // ========================================================================
    // SEARCH ENGINE
    // ========================================================================

    auto searchEngine =
        std::make_shared<SearchEngine>(
            window,
            original_model,
            searchIndex
        );

        // ========================================================================
// QUIT SLYNT
//
// Por ahora hacemos un cierre simple:
// 1. Detenemos el reproductor.
// 2. Terminamos completamente el proceso.
//
// Esto también cierra cualquier ventana secundaria,
// incluyendo Library y About.
// ========================================================================
 window->on_quit_all(
    [player] {

        player->stop();

        std::exit(0);
    }
);

// ========================================================================
// OPEN ABOUT
// ========================================================================

auto aboutWindow =
    std::make_shared<
        std::optional<slint::ComponentHandle<AboutWindow>>
    >();

window->on_open_about(
    [
        aboutWindow
    ]() mutable {

        if (aboutWindow->has_value()) {
            aboutWindow->value()->show();
            return;
        }

        auto about = AboutWindow::create();

        about->on_close(
            [
                aboutWindow
            ] {
                if (aboutWindow->has_value()) {
                    aboutWindow->value()->hide();
                }
            }
        );

        about->on_open_github(
            [] {
                std::system(
                    "xdg-open https://github.com >/dev/null 2>&1"
                );
            }
        );

        *aboutWindow = about;

        aboutWindow->value()->show();
    }
);

    // ========================================================================
    // OPEN LIBRARY
    // ========================================================================

    window->on_open_library(
        [
            window,
            libraryBridge,
            allTracks,
            original_model,
            searchIndex,
            player,
            getCover,
            setDsdUi,
            setPcmUi,
            shuffleEnabled,
            shuffledOrder,
            shuffledPos
        ]() mutable {

            libraryBridge->Show(
                [
                    window,
                    allTracks,
                    original_model,
                    searchIndex,
                    player,
                    getCover,
                    setDsdUi,
                    setPcmUi,
                    shuffleEnabled,
                    shuffledOrder,
                    shuffledPos
                ](
                    std::vector<TrackDataLite> lite
                ) mutable {

                    allTracks->clear();

                    original_model->clear();


                    int id = 0;


                    for (auto& t : lite) {

                        std::string title =
                            t.title.empty()
                                ? std::filesystem::path(
                                    t.path
                                  ).stem().string()
                                : t.title;


                        if (title.empty())
                            title =
                                "Track "
                                + std::to_string(id);


                        std::string artist =
                            t.artist.empty()
                                ? "Unknown artist"
                                : t.artist;


                        std::string album =
                            t.album.empty()
                                ? "Unknown album"
                                : t.album;


                        std::string blob =
                            toLower(
                                title
                                + " "
                                + artist
                                + " "
                                + album
                            );


                        bool dsd =
                            isDsdPath(
                                t.path
                            );


                        allTracks->push_back(
                            {
                                t.path,
                                title,
                                artist,
                                album,
                                t.duration,
                                blob,
                                t.year,
                                id,
                                44100,
                                24,
                                2,
                                dsd
                            }
                        );


                        Track st;


                        st.id = id;


                        st.title =
                            slint::SharedString(
                                title
                            );


                        st.path =
                            slint::SharedString(
                                t.path
                            );


                        st.artist =
                            slint::SharedString(
                                artist
                            );


                        st.album =
                            slint::SharedString(
                                album
                            );


                        st.year =
                            slint::SharedString(
                                t.year
                            );


                        st.duration =
                            slint::SharedString(
                                t.duration
                            );


                        st.codec =
                            slint::SharedString(
                                dsd
                                    ? "DSF"
                                    : "FLAC"
                            );


                        original_model->push_back(
                            st
                        );


                        id++;
                    }


                    // --------------------------------------------------------
                    // PUBLICAR SNAPSHOT DE BÚSQUEDA
                    // --------------------------------------------------------

                    auto snapshot =
                        std::make_shared<
                            const std::vector<TrackData>
                        >(
                            *allTracks
                        );


                    std::atomic_store(
                        searchIndex.get(),
                        snapshot
                    );


                    window->set_filtered(
                        original_model
                    );


                    shuffledOrder->clear();


                    for (
                        int i = 0;
                        i < (int)allTracks->size();
                        i++
                    ) {

                        shuffledOrder->push_back(
                            i
                        );
                    }


                    *shuffleEnabled = false;

                    window->set_shuffle(
                        false
                    );


                    if (!allTracks->empty()) {

                        auto td =
                            (*allTracks)[0];


                        slint::invoke_from_event_loop(
                            [
                                window,
                                td,
                                player,
                                getCover,
                                setDsdUi,
                                setPcmUi
                            ] {

                                auto dac =
                                    detectDac();


                                window->set__current_id(
                                    -1
                                );


                                window->set__title(
                                    slint::SharedString(
                                        td.title
                                    )
                                );


                                window->set__artist(
                                    slint::SharedString(
                                        td.artist
                                    )
                                );


                                window->set__album(
                                    slint::SharedString(
                                        td.album
                                    )
                                );
                            }
                        );
                    }
                }
            );
        }
    );


    // ========================================================================
    // FULLSCREEN
    // ========================================================================

    window->on_toggle_fullscreen(
        [window] {

            static bool fs = false;

            fs = !fs;

            window->window().set_fullscreen(
                fs
            );
        }
    );


    // ========================================================================
    // PLAY / PAUSE
    // ========================================================================

    window->on_play_pause(
        [player, window] {

            if (detectDac().device.empty())
                return;


            bool isPlaying =
                window->get__is_playing();


            if (isPlaying) {

                player->pause();

                window->set__is_playing(
                    false
                );

            } else {

                player->play();

                window->set__is_playing(
                    true
                );
            }
        }
    );


    // ========================================================================
    // NEXT / PREVIOUS
    // ========================================================================

    window->on_next(
        [playNextFn] {
            playNextFn();
        }
    );


    window->on_prev(
        [playPrevFn] {
            playPrevFn();
        }
    );


    // ========================================================================
    // SHUFFLE
    // ========================================================================

    window->on_toggle_shuffle(
        [
            window,
            allTracks,
            shuffleEnabled,
            shuffledOrder,
            shuffledPos
        ] {

            *shuffleEnabled =
                !*shuffleEnabled;


            window->set_shuffle(
                *shuffleEnabled
            );


            if (*shuffleEnabled) {

                shuffledOrder->clear();


                for (
                    int i = 0;
                    i < (int)allTracks->size();
                    i++
                ) {

                    shuffledOrder->push_back(
                        i
                    );
                }


                std::random_device rd;

                std::mt19937 g(rd());


                std::shuffle(
                    shuffledOrder->begin(),
                    shuffledOrder->end(),
                    g
                );


                int cur =
                    window->get__current_id();


                for (
                    int i = 0;
                    i < (int)allTracks->size();
                    i++
                ) {

                    if (
                        (*allTracks)[i].id
                        ==
                        cur
                    ) {

                        auto it =
                            std::find(
                                shuffledOrder->begin(),
                                shuffledOrder->end(),
                                i
                            );


                        if (
                            it
                            !=
                            shuffledOrder->end()
                        ) {

                            std::iter_swap(
                                shuffledOrder->begin(),
                                it
                            );
                        }


                        break;
                    }
                }


                *shuffledPos = 0;
            }
        }
    );


    // ========================================================================
    // REPEAT
    // ========================================================================

    window->on_toggle_repeat(
        [window, repeatMode] {

            if (*repeatMode == "Off")
                *repeatMode = "All";

            else if (*repeatMode == "All")
                *repeatMode = "One";

            else
                *repeatMode = "Off";


            window->set_repeat_mode(
                slint::SharedString(
                    *repeatMode
                )
            );
        }
    );


    // ========================================================================
    // PLAY TRACK
    // ========================================================================

    window->on_play_track(
        [playAtId](int trackId) {

            playAtId(trackId);
        }
    );


    // ========================================================================
    // OUTPUT MODE
    // ========================================================================

    window->on_set_output_mode(
        [player, window](
            slint::SharedString m
        ) {

            std::string s(
                m.data(),
                m.size()
            );


            OutputMode mode =
                OutputMode::Exclusive;


            if (s == "compatible")
                mode =
                    OutputMode::Compatible;


            player->setOutputMode(
                mode
            );


            slint::invoke_from_event_loop(
                [
                    window,
                    player,
                    mode
                ] {

                    window->set__device(
                        slint::SharedString(
                            player->hwDevice.empty()
                                ? "No DAC"
                                : player->hwDevice
                        )
                    );


                    window->set__backend(
                        slint::SharedString(
                            mode
                                ==
                                OutputMode::Compatible
                                    ? "System Audio"
                                    : "ALSA Direct"
                        )
                    );


                    window->set__dac(
                        slint::SharedString(
                            detectDac().name
                        )
                    );


                    if (
                        !player->currentPath.empty()
                    ) {

                        window->set__bit_perfect(
                            player->isBitPerfectFor(
                                player->currentPath
                            )
                        );
                    }
                }
            );
        }
    );


    // ========================================================================
    // OPEN FOLDER
    // ========================================================================

    window->on_open_folder(
        [
            player,
            window,
            allTracks,
            searchIndex,
            shuffledOrder,
            shuffleEnabled,
            original_model
        ]() mutable {

            std::string folder;


            char buf[2048] = {0};


            FILE* f =
                popen(
                    "zenity --file-selection --directory --title='Open music folder' 2>/dev/null",
                    "r"
                );


            if (f) {

                if (
                    fgets(
                        buf,
                        sizeof(buf),
                        f
                    )
                ) {

                    folder = buf;

                    folder.erase(
                        folder.find_last_not_of(
                            "\n\r"
                        )
                        + 1
                    );
                }


                pclose(f);
            }


            if (folder.empty())
                return;


            allTracks->clear();

            original_model->clear();

            shuffledOrder->clear();

            *shuffleEnabled = false;


            int id = 0;


            for (
                auto& p :
                std::filesystem::recursive_directory_iterator(
                    folder,
                    std::filesystem::directory_options::skip_permission_denied
                )
            ) {

                if (!p.is_regular_file())
                    continue;


                auto ext =
                    p.path()
                      .extension()
                      .string();


                std::transform(
                    ext.begin(),
                    ext.end(),
                    ext.begin(),
                    ::tolower
                );


                if (
                    ext != ".flac"
                    &&
                    ext != ".wav"
                    &&
                    ext != ".dsf"
                    &&
                    ext != ".dff"
                    &&
                    ext != ".aiff"
                    &&
                    ext != ".mp3"
                    &&
                    ext != ".m4a"
                    &&
                    ext != ".ogg"
                    &&
                    ext != ".opus"
                )
                    continue;


                std::string title =
                    p.path()
                     .stem()
                     .string();


                std::string artist =
                    "Unknown artist";


                std::string album =
                    "Unknown album";


                std::string year = "";

                std::string duration =
                    "--:--";


                int sr = 44100;

                int ch = 2;

                int bits = 16;


                TagLib::FileRef fr(
                    p.path().c_str()
                );


                if (
                    !fr.isNull()
                    &&
                    fr.tag()
                ) {

                    if (
                        !fr.tag()
                         ->title()
                         .isEmpty()
                    ) {

                        title =
                            fr.tag()
                              ->title()
                              .to8Bit(true);
                    }


                    if (
                        !fr.tag()
                         ->artist()
                         .isEmpty()
                    ) {

                        artist =
                            fr.tag()
                              ->artist()
                              .to8Bit(true);
                    }


                    if (
                        !fr.tag()
                         ->album()
                         .isEmpty()
                    ) {

                        album =
                            fr.tag()
                              ->album()
                              .to8Bit(true);
                    }


                    if (
                        fr.tag()->year()
                        > 0
                    ) {

                        year =
                            std::to_string(
                                fr.tag()->year()
                            );
                    }
                }


                if (
                    !fr.isNull()
                    &&
                    fr.audioProperties()
                ) {

                    sr =
                        fr.audioProperties()
                          ->sampleRate();


                    ch =
                        fr.audioProperties()
                          ->channels();


                    int secs =
                        fr.audioProperties()
                          ->length();


                    if (secs > 0) {

                        char dbuf[16];


                        snprintf(
                            dbuf,
                            sizeof(dbuf),
                            "%d:%02d",
                            secs / 60,
                            secs % 60
                        );


                        duration =
                            std::string(dbuf);
                    }
                }


                if (
                    ext == ".flac"
                    ||
                    ext == ".wav"
                ) {

                    bits = 24;
                }


                bool dsd =
                    ext == ".dsf"
                    ||
                    ext == ".dff";


                if (dsd) {

                    sr = 2822400;

                    ch = 2;

                    bits = 1;


                    if (
                        duration
                        ==
                        "--:--"
                    ) {

                        duration = "DSD";
                    }
                }


                std::string blob =
                    toLower(
                        title
                        + " "
                        + artist
                        + " "
                        + album
                    );


                allTracks->push_back(
                    {
                        p.path().string(),
                        title,
                        artist,
                        album,
                        duration,
                        blob,
                        year,
                        id,
                        sr,
                        bits,
                        ch,
                        dsd
                    }
                );


                Track t;


                t.id = id;


                t.title =
                    slint::SharedString(
                        title
                    );


                t.path =
                    slint::SharedString(
                        p.path().string()
                    );


                t.codec =
                    slint::SharedString(
                        dsd
                            ? "DSF"
                            : "FLAC"
                    );


                t.duration =
                    slint::SharedString(
                        duration
                    );


                t.artist =
                    slint::SharedString(
                        artist
                    );


                t.album =
                    slint::SharedString(
                        album
                    );


                t.year =
                    slint::SharedString(
                        year
                    );


                original_model->push_back(
                    t
                );


                id++;
            }


            // --------------------------------------------------------
            // ACTUALIZAR ÍNDICE DEL BUSCADOR
            // --------------------------------------------------------

            auto snapshot =
                std::make_shared<
                    const std::vector<TrackData>
                >(
                    *allTracks
                );


            std::atomic_store(
                searchIndex.get(),
                snapshot
            );


            window->set_filtered(
                original_model
            );


            window->set_shuffle(
                false
            );


            slint::invoke_from_event_loop(
                [window] {

                    window->set__title(
                        slint::SharedString(
                            "No track selected"
                        )
                    );


                    window->set__current_id(
                        -1
                    );


                    window->set__is_playing(
                        false
                    );


                    window->set__cover(
                        slint::Image()
                    );


                    window->set_shuffle(
                        false
                    );


                    auto idle =
                        std::make_shared<
                            slint::VectorModel<float>
                        >();


                    for (
                        int i = 0;
                        i < 32;
                        i++
                    ) {

                        idle->push_back(
                            0.0f
                        );
                    }


                    window->set__spectrum(
                        idle
                    );
                }
            );
        }
    );


    // ========================================================================
    // SEARCH
    // ========================================================================

    window->on_search(
        [searchEngine](
            slint::SharedString query
        ) {

            searchEngine->request(
                std::string(
                    query.data(),
                    query.size()
                )
            );
        }
    );


    // ========================================================================
    // FILTER BY ARTIST
    // ========================================================================

    window->on_filter_by_artist(
        [
            window,
            allTracks
        ](
            slint::SharedString artist_s
        ) {

            std::string artist_q(
                artist_s.data(),
                artist_s.size()
            );


            auto filtered =
                std::make_shared<
                    slint::VectorModel<Track>
                >();


            for (auto& td : *allTracks) {

                if (
                    td.artist
                    !=
                    artist_q
                )
                    continue;


                Track t;


                t.id = td.id;


                t.title =
                    slint::SharedString(
                        td.title
                    );


                t.path =
                    slint::SharedString(
                        td.path
                    );


                t.codec =
                    slint::SharedString(
                        td.is_dsd
                            ? "DSF"
                            : "FLAC"
                    );


                t.duration =
                    slint::SharedString(
                        td.duration
                    );


                t.artist =
                    slint::SharedString(
                        td.artist
                    );


                t.album =
                    slint::SharedString(
                        td.album
                    );


                t.year =
                    slint::SharedString(
                        td.year
                    );


                filtered->push_back(
                    t
                );
            }


            window->set_filtered(
                filtered
            );
        }
    );


    // ========================================================================
    // CLEAR FILTER
    // ========================================================================

    window->on_clear_filter(
        [
            window,
            original_model
        ] {

            window->set_filtered(
                original_model
            );
        }
    );


    // ========================================================================
    // CLEAR PLAYLIST
    // ========================================================================

    window->on_clear_playlist(
        [
            window,
            allTracks,
            player,
            shuffledOrder,
            shuffleEnabled,
            repeatMode,
            original_model,
            searchIndex
        ]() mutable {

            player->stop();


            allTracks->clear();


            original_model->clear();


            // Índice vacío
            std::atomic_store(
                searchIndex.get(),
                std::make_shared<
                    const std::vector<TrackData>
                >()
            );


            window->set_filtered(
                original_model
            );


            shuffledOrder->clear();


            *shuffleEnabled = false;

            *repeatMode = "Off";


            window->set__title(
                slint::SharedString(
                    "Playlist cleared"
                )
            );


            window->set__artist(
                slint::SharedString(
                    "Open a folder to get started"
                )
            );


            window->set__album(
                slint::SharedString("—")
            );


            window->set__current_id(
                -1
            );


            window->set__is_playing(
                false
            );


            window->set__progress(
                0
            );


            window->set__time_pos(
                slint::SharedString(
                    "0:00"
                )
            );


            window->set__time_total(
                slint::SharedString(
                    "0:00"
                )
            );


            window->set__samplerate(
                slint::SharedString(
                    "— kHz"
                )
            );


            window->set__bitdepth(
                slint::SharedString(
                    "—-bit"
                )
            );


            window->set__channels(
                slint::SharedString(
                    "— ch"
                )
            );


            window->set__signal_path(
                slint::SharedString("—")
            );


            window->set__codec(
                slint::SharedString("—")
            );


            window->set__cover(
                slint::Image()
            );


            window->set_shuffle(
                false
            );


            window->set_repeat_mode(
                slint::SharedString(
                    "Off"
                )
            );


            auto idle =
                std::make_shared<
                    slint::VectorModel<float>
                >();


            for (
                int i = 0;
                i < 32;
                i++
            ) {

                idle->push_back(
                    0.0f
                );
            }


            window->set__spectrum(
                idle
            );


            auto dac =
                detectDac();


            window->set__dac(
                slint::SharedString(
                    dac.name
                )
            );


            window->set__device(
                slint::SharedString(
                    dac.device.empty()
                        ? "No DAC"
                        : dac.device
                          + " • idle"
                )
            );
        }
    );


    // ========================================================================
    // DELETE TRACK
    // ========================================================================

    window->on_delete_track(
        [
            window,
            allTracks,
            player,
            original_model,
            searchIndex
        ](int trackId) mutable {

            auto it =
                std::find_if(
                    allTracks->begin(),
                    allTracks->end(),
                    [trackId](auto& t) {

                        return t.id
                            ==
                            trackId;
                    }
                );


            if (
                it
                ==
                allTracks->end()
            )
                return;


            bool isCurrent =
                window->get__current_id()
                ==
                trackId;


            if (isCurrent)
                player->stop();


            allTracks->erase(it);


            original_model->clear();


            for (auto& td : *allTracks) {

                Track t;


                t.id = td.id;


                t.title =
                    slint::SharedString(
                        td.title
                    );


                t.path =
                    slint::SharedString(
                        td.path
                    );


                t.codec =
                    slint::SharedString(
                        td.is_dsd
                            ? "DSF"
                            : "FLAC"
                    );


                t.duration =
                    slint::SharedString(
                        td.duration
                    );


                t.artist =
                    slint::SharedString(
                        td.artist
                    );


                t.album =
                    slint::SharedString(
                        td.album
                    );


                t.year =
                    slint::SharedString(
                        td.year
                    );


                original_model->push_back(
                    t
                );
            }


            // --------------------------------------------------------
            // ACTUALIZAR ÍNDICE
            // --------------------------------------------------------

            auto snapshot =
                std::make_shared<
                    const std::vector<TrackData>
                >(
                    *allTracks
                );


            std::atomic_store(
                searchIndex.get(),
                snapshot
            );


            window->set_filtered(
                original_model
            );


            if (isCurrent) {

                window->set__current_id(
                    -1
                );


                window->set__is_playing(
                    false
                );


                window->set__title(
                    slint::SharedString(
                        "Track removed"
                    )
                );


                window->set__progress(
                    0
                );


                window->set__cover(
                    slint::Image()
                );


                auto idle =
                    std::make_shared<
                        slint::VectorModel<float>
                    >();


                for (
                    int i = 0;
                    i < 32;
                    i++
                ) {

                    idle->push_back(
                        0.0f
                    );
                }


                window->set__spectrum(
                    idle
                );
            }
        }
    );


    // ========================================================================
    // SEEK
    // ========================================================================

    window->on_seek(
        [player](float p) {

            if (
                player->getDuration()
                >
                0
            ) {

                player->seek(p);
            }
        }
    );


    // ========================================================================
    // VOLUME
    // ========================================================================

    window->on_set_volume(
        [player](float v) {

            player->setVolume(
                std::clamp(
                    v,
                    0.0f,
                    1.0f
                )
            );
        }
    );


    // ========================================================================
    // STATUS THREAD
    // ========================================================================

    std::jthread(
        [player, window] {

            while (true) {

                std::this_thread::sleep_for(
                    std::chrono::milliseconds(
                        400
                    )
                );


                auto dac =
                    detectDac();


                if (
                    dac.device.empty()
                ) {

                    slint::invoke_from_event_loop(
                        [window] {

                            window->set__dac(
                                slint::SharedString(
                                    "No DAC - apagado"
                                )
                            );


                            window->set__device(
                                slint::SharedString(
                                    "Connect the USB DAC"
                                )
                            );


                            window->set__backend(
                                slint::SharedString(
                                    "---"
                                )
                            );


                            window->set__is_playing(
                                false
                            );
                        }
                    );


                    continue;
                }


                double pos =
                    player->getPosition();


                double dur =
                    player->getDuration();


                std::string devToCheck =
                    player->hwDevice.empty()
                        ? dac.device
                        : player->hwDevice;


                auto live =
                    getLiveHwParams(
                        devToCheck
                    );


                if (
                    dur > 0.5
                    ||
                    pos >= 0
                ) {

                    float pr =
                        dur > 1.0
                            ? pos / dur
                            : 0.0f;


                    char b1[16];

                    char b2[16];


                    snprintf(
                        b1,
                        sizeof(b1),
                        "%d:%02d",
                        (int)pos / 60,
                        (int)pos % 60
                    );


                    if (dur > 1.0) {

                        snprintf(
                            b2,
                            sizeof(b2),
                            "%d:%02d",
                            (int)dur / 60,
                            (int)dur % 60
                        );

                    } else {

                        snprintf(
                            b2,
                            sizeof(b2),
                            "%s",
                            window
                                ->get__time_total()
                                .data()
                        );
                    }


                    slint::invoke_from_event_loop(
                        [
                            window,
                            pr,
                            b1 = std::string(b1),
                            b2 = std::string(b2),
                            dac,
                            live,
                            devToCheck,
                            player
                        ] {

                            if (
                                pr >= 0
                                &&
                                pr <= 1.0
                            ) {

                                window->set__progress(
                                    pr
                                );
                            }


                            window->set__time_pos(
                                slint::SharedString(
                                    b1
                                )
                            );


                            if (
                                std::string(b2)
                                    .find("--")
                                ==
                                std::string::npos
                                &&
                                std::string(b2)
                                    .size()
                                >
                                0
                            ) {

                                window->set__time_total(
                                    slint::SharedString(
                                        b2
                                    )
                                );
                            }


                            if (live.active) {

                                bool dsd =
                                    isDsdPath(
                                        player->currentPath
                                    );


                                if (dsd) {

                                    std::string dsdRate =
                                        live.rateHz >= 300000
                                            ? "5.6 MHz (DoP 352.8 kHz)"
                                            : "2.8 MHz (DoP 176.4 kHz)";


                                    window->set__samplerate(
                                        slint::SharedString(
                                            dsdRate
                                        )
                                    );


                                    window->set__bitdepth(
                                        slint::SharedString(
                                            "DSD • "
                                            + live.format
                                        )
                                    );


                                    window->set__signal_path(
                                        slint::SharedString(
                                            "DSD -> DoP -> "
                                            + dac.name
                                        )
                                    );


                                    window->set__device(
                                        slint::SharedString(
                                            devToCheck
                                            + " • DoP"
                                        )
                                    );

                                } else {

                                    window->set__samplerate(
                                        slint::SharedString(
                                            live.rate
                                        )
                                    );


                                    window->set__bitdepth(
                                        slint::SharedString(
                                            live.format
                                        )
                                    );


                                    window->set__device(
                                        slint::SharedString(
                                            devToCheck
                                            + " • "
                                            + live.access
                                        )
                                    );
                                }


                                window->set__dac(
                                    slint::SharedString(
                                        dac.name
                                        + " [LIVE]"
                                    )
                                );


                                window->set__bit_perfect(
                                    true
                                );

                            }
                        }
                    );

                } else {

                    slint::invoke_from_event_loop(
                        [
                            window,
                            dac,
                            live,
                            devToCheck
                        ] {

                            window->set__dac(
                                slint::SharedString(
                                    dac.name
                                )
                            );


                            window->set__backend(
                                slint::SharedString(
                                    dac.backend
                                )
                            );


                            if (!live.active) {

                                window->set__device(
                                    slint::SharedString(
                                        devToCheck
                                        + " • idle"
                                    )
                                );
                            }
                        }
                    );
                }
            }
        }
    ).detach();


    // ========================================================================
    // RUN
    // ========================================================================

    window->run();


    return 0;
}


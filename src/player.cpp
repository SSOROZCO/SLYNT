#include "player.h"

#include <gst/gst.h>
#include <slint.h>

#include <iostream>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <climits>
#include <filesystem>

// ============================================================
// PLAYER IMPLEMENTATION
// ============================================================

struct SimplePlayer::PlayerImpl {

    GstElement *playbin = nullptr;
    GstBus *bus = nullptr;
    guint busWatch = 0;

    bool playing = false;
};

// ============================================================
// DETECCIÓN USB DAC
// ============================================================

static std::string getFirstUsbCardName()
{
    std::ifstream f("/proc/asound/cards");

    std::string line;

    while (std::getline(f, line)) {

        if (line.find("USB-Audio") != std::string::npos) {

            auto l = line.find("[");
            auto r = line.find("]");

            if (l != std::string::npos &&
                r != std::string::npos) {

                std::string name =
                    line.substr(
                        l + 1,
                        r - l - 1
                    );

                name.erase(
                    0,
                    name.find_first_not_of(" \t")
                );

                name.erase(
                    name.find_last_not_of(" \t\r\n") + 1
                );

                return name;
            }
        }
    }

    return "";
}

// ============================================================
// DEVICE STRING
// ============================================================

static std::string getExclusiveDeviceString()
{
    std::string cardName =
        getFirstUsbCardName();

    if (!cardName.empty()) {

        return
            "hw:CARD=" +
            cardName +
            ",DEV=0";
    }

    return "";
}

// ============================================================
// SPECTRUM + ALSA DIRECT
// ============================================================

static GstElement* makeSpectrumBin(
    const std::string &deviceStr,
    const std::string &binName = "audio-bin"
)
{
    GstElement *bin =
        gst_element_factory_make(
            "bin",
            binName.c_str()
        );

    GstElement *tee =
        gst_element_factory_make(
            "tee",
            "tee"
        );

    GstElement *q1 =
        gst_element_factory_make(
            "queue",
            "q1"
        );

    GstElement *q2 =
        gst_element_factory_make(
            "queue",
            "q2"
        );

    GstElement *conv =
        gst_element_factory_make(
            "audioconvert",
            "conv"
        );

    GstElement *spectrum =
        gst_element_factory_make(
            "spectrum",
            "spec"
        );

    GstElement *sink =
        gst_element_factory_make(
            "alsasink",
            "sink"
        );

    GstElement *fake =
        gst_element_factory_make(
            "fakesink",
            "fake"
        );

    if (!bin ||
        !tee ||
        !q1 ||
        !q2 ||
        !conv ||
        !spectrum ||
        !sink ||
        !fake) {

        std::cerr
            << "[SLYNT] Failed to create spectrum pipeline"
            << std::endl;

        if (sink)
            return sink;

        return nullptr;
    }

    // ========================================================
    // SPECTRUM
    // ========================================================

    g_object_set(
        spectrum,

        "bands",
        32,

        "threshold",
        -70,

        "interval",
        40000000ULL,

        "post-messages",
        TRUE,

        "message-magnitude",
        TRUE,

        "message-phase",
        FALSE,

        nullptr
    );

    // ========================================================
    // ALSA SINK
    // ========================================================

    g_object_set(
        sink,

        "device",
        deviceStr.c_str(),

        "sync",
        FALSE,

        nullptr
    );

    // ========================================================
    // FAKE SINK
    // ========================================================

    g_object_set(
        fake,

        "sync",
        FALSE,

        nullptr
    );

    // ========================================================
    // QUEUE SPECTRUM
    // ========================================================

    g_object_set(
        q1,

        "leaky",
        1,

        "max-size-time",
        100000000ULL,

        "max-size-buffers",
        0,

        "max-size-bytes",
        0,

        nullptr
    );

    // ========================================================
    // QUEUE AUDIO
    // ========================================================

    g_object_set(
        q2,

        "max-size-time",
        200000000ULL,

        "max-size-buffers",
        0,

        "max-size-bytes",
        0,

        nullptr
    );

    // ========================================================
    // ADD
    // ========================================================

    gst_bin_add_many(
        GST_BIN(bin),

        tee,
        q1,
        q2,
        conv,
        spectrum,
        sink,
        fake,

        nullptr
    );

    // ========================================================
    // SPECTRUM BRANCH
    // ========================================================

    if (!gst_element_link_many(
            tee,
            q1,
            conv,
            spectrum,
            fake,
            nullptr)) {

        std::cerr
            << "[SLYNT] Failed to link spectrum branch"
            << std::endl;
    }

    // ========================================================
    // AUDIO BRANCH
    // ========================================================

    if (!gst_element_link_many(
            tee,
            q2,
            sink,
            nullptr)) {

        std::cerr
            << "[SLYNT] Failed to link ALSA branch"
            << std::endl;
    }

    // ========================================================
    // GHOST PAD
    // ========================================================

    GstPad *teeSink =
        gst_element_get_static_pad(
            tee,
            "sink"
        );

    if (!teeSink) {

        std::cerr
            << "[SLYNT] Failed to obtain tee sink pad"
            << std::endl;

        return bin;
    }

    GstPad *ghost =
        gst_ghost_pad_new(
            "sink",
            teeSink
        );

    gst_element_add_pad(
        bin,
        ghost
    );

    gst_object_unref(
        teeSink
    );

    return bin;
}

// ============================================================
// CREATE SINK
//
// AUTO:
//   DAC disponible -> ALSA Direct / BIT-PERFECT
//   DAC ausente   -> System Audio / PCM
//
// COMPATIBLE:
//   siempre System Audio / PCM
//
// EXCLUSIVE:
//   DAC requerido
// ============================================================

GstElement* SimplePlayer::createSink(
    const std::string& path
)
{
    std::string usbCard =
        getFirstUsbCardName();

    std::string exclusiveDev =
        getExclusiveDeviceString();

    bool hasUsbDac =
        !exclusiveDev.empty();

    // ========================================================
    // COMPATIBLE
    // ========================================================

    if (outputMode ==
        OutputMode::Compatible) {

        currentDevice =
            "System Audio (PCM) [COMPATIBLE]";

        hwDevice =
            "autoaudiosink";

        if (onSpectrum) {

            std::vector<float> empty(
                32,
                0.0f
            );

            onSpectrum(empty);
        }

        std::cout
            << "[SLYNT] Sink -> autoaudiosink"
            << " => "
            << currentDevice
            << std::endl;

        return gst_element_factory_make(
            "autoaudiosink",
            "sink"
        );
    }

    // ========================================================
    // SIN DAC
    // ========================================================

    if (!hasUsbDac) {

        // ----------------------------------------------------
        // AUTO
        // ----------------------------------------------------

        if (outputMode ==
            OutputMode::Auto) {

            currentDevice =
                "System Audio (PCM) [AUTO]";

            hwDevice =
                "autoaudiosink";

            if (onSpectrum) {

                std::vector<float> empty(
                    32,
                    0.0f
                );

                onSpectrum(empty);
            }

            std::cout
                << "[SLYNT] No USB DAC detected"
                << " - Auto fallback -> PCM"
                << std::endl;

            return gst_element_factory_make(
                "autoaudiosink",
                "sink"
            );
        }

        // ----------------------------------------------------
        // EXCLUSIVE SIN DAC
        // ----------------------------------------------------

        currentDevice =
            "System Audio (PCM) [NO DAC]";

        hwDevice =
            "autoaudiosink";

        if (onSpectrum) {

            std::vector<float> empty(
                32,
                0.0f
            );

            onSpectrum(empty);
        }

        std::cout
            << "[SLYNT] No USB DAC detected"
            << " - fallback -> PCM"
            << std::endl;

        return gst_element_factory_make(
            "autoaudiosink",
            "sink"
        );
    }

    // ========================================================
    // DAC DISPONIBLE
    // ========================================================

    bool isDSD =
        path.ends_with(".dsf") ||
        path.ends_with(".dff") ||
        path.ends_with(".DSF") ||
        path.ends_with(".DFF");

    std::string devToUse;

    if (isDSD) {

        devToUse =
            "plughw:CARD=" +
            usbCard +
            ",DEV=0";

        currentDevice =
            usbCard +
            " (" +
            devToUse +
            ") [DSD-DoP] [BIT-PERFECT]";

    } else {

        devToUse =
            exclusiveDev;

        currentDevice =
            usbCard +
            " (" +
            devToUse +
            ") [BIT-PERFECT]";
    }

    hwDevice =
        devToUse;

    std::cout
        << "[SLYNT] Sink -> "
        << devToUse
        << " => "
        << currentDevice
        << " + SPECTRUM"
        << std::endl;

    return makeSpectrumBin(
        devToUse
    );
}

// ============================================================
// DEVICE BUSY / DAC DESCONECTADO
// ============================================================

void SimplePlayer::handleDeviceBusy()
{
    // ========================================================
    // DAC DESAPARECIÓ
    // ========================================================

    if (getFirstUsbCardName().empty()) {

        std::cout
            << "[DAC] USB DAC disconnected"
            << " - Auto fallback -> PCM"
            << std::endl;

        // ----------------------------------------------------
        // AUTO
        // ----------------------------------------------------

        if (outputMode ==
            OutputMode::Auto) {

            gint64 pos = 0;

            gst_element_query_position(
                impl->playbin,
                GST_FORMAT_TIME,
                &pos
            );

            bool wasPlaying =
                impl->playing;

            gst_element_set_state(
                impl->playbin,
                GST_STATE_NULL
            );

            GstElement* sink =
                createSink(currentPath);

            g_object_set(
                impl->playbin,
                "audio-sink",
                sink,
                nullptr
            );

            if (!currentPath.empty()) {

                g_object_set(
                    impl->playbin,
                    "uri",
                    ("file://" +
                     currentPath).c_str(),
                    nullptr
                );

                gst_element_set_state(
                    impl->playbin,
                    GST_STATE_PAUSED
                );

                gst_element_get_state(
                    impl->playbin,
                    nullptr,
                    nullptr,
                    2 * GST_SECOND
                );

                if (pos > 0) {

                    gst_element_seek_simple(
                        impl->playbin,
                        GST_FORMAT_TIME,

                        (GstSeekFlags)(
                            GST_SEEK_FLAG_FLUSH |
                            GST_SEEK_FLAG_KEY_UNIT
                        ),

                        pos
                    );
                }

                if (wasPlaying) {

                    gst_element_set_state(
                        impl->playbin,
                        GST_STATE_PLAYING
                    );

                    impl->playing = true;
                }
            }

            if (onSpectrum) {

                onSpectrum(
                    std::vector<float>(
                        32,
                        0.0f
                    )
                );
            }

            return;
        }

        // ----------------------------------------------------
        // NO AUTO
        // ----------------------------------------------------

        stop();

        return;
    }

    // ========================================================
    // DAC SIGUE PRESENTE
    // ========================================================

    std::cerr
        << "[DAC] "
        << hwDevice
        << " busy -> suspending PipeWire"
        << std::endl;

    system(
        "pactl suspend-sink @DEFAULT_SINK@ 1 "
        "2>/dev/null; sleep 0.2"
    );

    gint64 pos = 0;

    gst_element_query_position(
        impl->playbin,
        GST_FORMAT_TIME,
        &pos
    );

    bool wasPlaying =
        impl->playing;

    gst_element_set_state(
        impl->playbin,
        GST_STATE_NULL
    );

    GstElement* sink =
        createSink(currentPath);

    g_object_set(
        impl->playbin,
        "audio-sink",
        sink,
        nullptr
    );

    if (!currentPath.empty()) {

        g_object_set(
            impl->playbin,
            "uri",
            ("file://" +
             currentPath).c_str(),
            nullptr
        );

        gst_element_set_state(
            impl->playbin,
            GST_STATE_PAUSED
        );

        gst_element_get_state(
            impl->playbin,
            nullptr,
            nullptr,
            2 * GST_SECOND
        );

        if (pos > 0) {

            gst_element_seek_simple(
                impl->playbin,
                GST_FORMAT_TIME,

                (GstSeekFlags)(
                    GST_SEEK_FLAG_FLUSH |
                    GST_SEEK_FLAG_KEY_UNIT
                ),

                pos
            );
        }

        if (wasPlaying) {

            gst_element_set_state(
                impl->playbin,
                GST_STATE_PLAYING
            );

            impl->playing = true;
        }
    }
}

// ============================================================
// CONSTRUCTOR
// ============================================================

SimplePlayer::SimplePlayer()
{
    impl =
        new PlayerImpl();

    gst_init(
        nullptr,
        nullptr
    );

    // ========================================================
    // PLAYBIN
    // ========================================================

    impl->playbin =
        gst_element_factory_make(
            "playbin",
            "player"
        );

    if (!impl->playbin) {

        std::cerr
            << "[SLYNT] ERROR: unable to create playbin"
            << std::endl;

        return;
    }

    // ========================================================
    // SOLO AUDIO
    // ========================================================

    g_object_set(
        impl->playbin,

        "flags",
        0x00000002,

        nullptr
    );

    // ========================================================
    // VIDEO -> FAKESINK
    // ========================================================

    GstElement* videoSink =
        gst_element_factory_make(
            "fakesink",
            "videosink"
        );

    if (videoSink) {

        g_object_set(
            videoSink,

            "sync",
            FALSE,

            "async",
            FALSE,

            nullptr
        );

        g_object_set(
            impl->playbin,
            "video-sink",
            videoSink,
            nullptr
        );
    }

    // ========================================================
    // AUDIO SINK INICIAL
    // ========================================================

    GstElement* sink =
        createSink("");

    if (sink) {

        g_object_set(
            impl->playbin,
            "audio-sink",
            sink,
            nullptr
        );
    }

    // ========================================================
    // VOLUMEN
    // ========================================================

    g_object_set(
        impl->playbin,

        "volume",
        0.85,

        nullptr
    );

    // ========================================================
    // READY
    // ========================================================

    gst_element_set_state(
        impl->playbin,
        GST_STATE_READY
    );

    // ========================================================
    // BUS
    // ========================================================

    impl->bus =
        gst_element_get_bus(
            impl->playbin
        );

    impl->busWatch =
        gst_bus_add_watch(
            impl->bus,

            [](GstBus*,
               GstMessage *msg,
               gpointer data) -> gboolean {

                SimplePlayer* self =
                    static_cast<SimplePlayer*>(
                        data
                    );

                // =================================================
                // SPECTRUM
                // =================================================

                if (
                    GST_MESSAGE_TYPE(msg)
                    ==
                    GST_MESSAGE_ELEMENT
                ) {

                    const GstStructure *s =
                        gst_message_get_structure(
                            msg
                        );

                    if (
                        s &&
                        gst_structure_has_name(
                            s,
                            "spectrum"
                        )
                    ) {

                        const GValue *list =
                            gst_structure_get_value(
                                s,
                                "magnitude"
                            );

                        if (list) {

                            std::vector<float> bars;

                            guint count =
                                gst_value_list_get_size(
                                    list
                                );

                            bars.reserve(
                                count
                            );

                            for (
                                guint i = 0;
                                i < count;
                                i++
                            ) {

                                const GValue *v =
                                    gst_value_list_get_value(
                                        list,
                                        i
                                    );

                                if (!v)
                                    continue;

                                float db =
                                    g_value_get_float(v);

                                float norm =
                                    (db + 70.0f) /
                                    70.0f;

                                norm =
                                    std::clamp(
                                        norm,
                                        0.0f,
                                        1.0f
                                    );

                                bars.push_back(
                                    norm
                                );
                            }

                            // =================================================
                            // MUY IMPORTANTE:
                            //
                            // ESTE CALLBACK VIENE DE GSTREAMER.
                            //
                            // NO TOCAMOS SLINT DIRECTAMENTE.
                            // =================================================

                            if (
                                self->onSpectrum &&
                                !bars.empty()
                            ) {

                                slint::invoke_from_event_loop(
                                    [self,
                                     bars = std::move(bars)]() mutable {

                                        if (
                                            self->onSpectrum
                                        ) {

                                            self->onSpectrum(
                                                bars
                                            );
                                        }
                                    }
                                );
                            }
                        }
                    }
                }

                // =================================================
                // EOS
                // =================================================

                if (
                    GST_MESSAGE_TYPE(msg)
                    ==
                    GST_MESSAGE_EOS
                ) {

                    self->consecutiveErrors =
                        0;

                    // UI SIEMPRE EN MAIN THREAD

                    slint::invoke_from_event_loop(
                        [self] {

                            if (
                                self->onSpectrum
                            ) {

                                self->onSpectrum(
                                    std::vector<float>(
                                        32,
                                        0.0f
                                    )
                                );
                            }

                            if (
                                self->onTrackFinished
                            ) {

                                self->onTrackFinished();
                            }
                        }
                    );
                }

                // =================================================
                // ERROR
                // =================================================

                if (
                    GST_MESSAGE_TYPE(msg)
                    ==
                    GST_MESSAGE_ERROR
                ) {

                    GError *err = nullptr;
                    gchar *dbg = nullptr;

                    gst_message_parse_error(
                        msg,
                        &err,
                        &dbg
                    );

                    std::cerr
                        << "[GStreamer] "
                        << (
                            err
                            ? err->message
                            : "unknown error"
                        )
                        << " - "
                        << (
                            dbg
                            ? dbg
                            : ""
                        )
                        << std::endl;

                    bool isBusy =
                        err &&
                        err->domain
                        ==
                        GST_RESOURCE_ERROR
                        &&
                        err->code
                        ==
                        GST_RESOURCE_ERROR_BUSY;

                    bool isTypeNotFound =
                        err &&
                        (
                            g_strrstr(
                                err->message,
                                "Could not determine type"
                            )
                            != nullptr
                        )
                        ||
                        err &&
                        (
                            g_strrstr(
                                err->message,
                                "Internal data stream error"
                            )
                            != nullptr
                        );

                    bool isDeviceError =
                        err &&
                        (
                            g_strrstr(
                                err->message,
                                "Could not open audio device"
                            )
                            != nullptr
                        )
                        ||
                        err &&
                        (
                            g_strrstr(
                                err->message,
                                "Device or resource busy"
                            )
                            != nullptr
                        )
                        ||
                        err &&
                        (
                            g_strrstr(
                                err->message,
                                "No such device"
                            )
                            != nullptr
                        );

                    if (err)
                        g_error_free(err);

                    if (dbg)
                        g_free(dbg);

                    // =================================================
                    // BUSY / DEVICE
                    // =================================================

                    if (
                        isBusy ||
                        isDeviceError
                    ) {

                        self->consecutiveErrors++;

                        if (
                            self->consecutiveErrors
                            < 3
                        ) {

                            slint::invoke_from_event_loop(
                                [self] {

                                    self->handleDeviceBusy();
                                }
                            );

                        } else {

                            slint::invoke_from_event_loop(
                                [self] {

                                    if (
                                        self->outputMode
                                        ==
                                        OutputMode::Auto
                                    ) {

                                        self->handleDeviceBusy();

                                    } else {

                                        self->stop();
                                    }
                                }
                            );
                        }
                    }

                    // =================================================
                    // ARCHIVO NO REPRODUCIBLE
                    // =================================================

                    else if (
                        isTypeNotFound
                    ) {

                        self->consecutiveErrors++;

                        std::cerr
                            << "[SLYNT] Archivo no reproducible,"
                            << " skip al siguiente"
                            << std::endl;

                        slint::invoke_from_event_loop(
                            [self] {

                                self->consecutiveErrors =
                                    0;

                                if (
                                    self->onTrackFinished
                                ) {

                                    self->onTrackFinished();
                                }
                            }
                        );
                    }

                    // =================================================
                    // OTRO ERROR
                    // =================================================

                    else {

                        slint::invoke_from_event_loop(
                            [self] {

                                self->consecutiveErrors =
                                    0;

                                if (
                                    self->onTrackFinished
                                ) {

                                    self->onTrackFinished();
                                }
                            }
                        );
                    }
                }

                return G_SOURCE_CONTINUE;
            },

            this
        );
}

// ============================================================
// DESTRUCTOR
// ============================================================

SimplePlayer::~SimplePlayer()
{
    if (impl->busWatch) {

        g_source_remove(
            impl->busWatch
        );

        impl->busWatch = 0;
    }

    if (impl->bus) {

        gst_object_unref(
            impl->bus
        );

        impl->bus = nullptr;
    }

    if (impl->playbin) {

        gst_element_set_state(
            impl->playbin,
            GST_STATE_NULL
        );

        gst_object_unref(
            impl->playbin
        );

        impl->playbin = nullptr;
    }

    delete impl;
}

// ============================================================
// BIT-PERFECT
// ============================================================

bool SimplePlayer::isBitPerfectFor(
    const std::string& p
)
{
    if (
        outputMode
        ==
        OutputMode::Compatible
    ) {

        return false;
    }

    if (
        hwDevice.empty()
    ) {

        return false;
    }

    return
        hwDevice.find("CARD=")
        !=
        std::string::npos;
}

// ============================================================
// OUTPUT MODE
// ============================================================

void SimplePlayer::setOutputMode(
    OutputMode mode
)
{
    if (
        outputMode == mode &&
        !currentPath.empty()
    ) {

        return;
    }

    outputMode =
        mode;

    gint64 pos = 0;

    gst_element_query_position(
        impl->playbin,
        GST_FORMAT_TIME,
        &pos
    );

    bool wasPlay =
        impl->playing;

    gst_element_set_state(
        impl->playbin,
        GST_STATE_NULL
    );

    GstElement* sink =
        createSink(
            currentPath
        );

    g_object_set(
        impl->playbin,
        "audio-sink",
        sink,
        nullptr
    );

    if (!currentPath.empty()) {

        g_object_set(
            impl->playbin,
            "uri",
            ("file://" +
             currentPath).c_str(),
            nullptr
        );

        gst_element_set_state(
            impl->playbin,
            GST_STATE_PAUSED
        );

        gst_element_get_state(
            impl->playbin,
            nullptr,
            nullptr,
            2 * GST_SECOND
        );

        if (pos > 0) {

            gst_element_seek_simple(
                impl->playbin,
                GST_FORMAT_TIME,

                (GstSeekFlags)(
                    GST_SEEK_FLAG_FLUSH |
                    GST_SEEK_FLAG_KEY_UNIT
                ),

                pos
            );
        }

        if (wasPlay) {

            gst_element_set_state(
                impl->playbin,
                GST_STATE_PLAYING
            );

            impl->playing =
                true;
        }
    }

    if (onOutputModeChanged) {

        onOutputModeChanged(
            mode
        );
    }
}

// ============================================================
// LOAD
// ============================================================

void SimplePlayer::load(
    const std::string& p
)
{
    currentPath =
        p;

    consecutiveErrors =
        0;

    gst_element_set_state(
        impl->playbin,
        GST_STATE_NULL
    );

    GstElement* sink =
        createSink(p);

    g_object_set(
        impl->playbin,
        "audio-sink",
        sink,
        nullptr
    );

    g_object_set(
        impl->playbin,
        "uri",
        ("file://" +
         p).c_str(),
        nullptr
    );

    gst_element_set_state(
        impl->playbin,
        GST_STATE_PAUSED
    );

    gst_element_get_state(
        impl->playbin,
        nullptr,
        nullptr,
        2 * GST_SECOND
    );

    if (onOutputModeChanged) {

        onOutputModeChanged(
            outputMode
        );
    }
}

// ============================================================
// PLAY
// ============================================================

void SimplePlayer::play()
{
    if (
        hwDevice.empty()
    ) {

        std::cout
            << "[SLYNT] play() - no audio sink"
            << std::endl;

        return;
    }

    gst_element_set_state(
        impl->playbin,
        GST_STATE_PLAYING
    );

    impl->playing =
        true;
}

// ============================================================
// PAUSE
// ============================================================

void SimplePlayer::pause()
{
    gst_element_set_state(
        impl->playbin,
        GST_STATE_PAUSED
    );

    impl->playing =
        false;
}

// ============================================================
// STOP
// ============================================================

void SimplePlayer::stop()
{
    gst_element_set_state(
        impl->playbin,
        GST_STATE_NULL
    );

    impl->playing =
        false;

    if (onSpectrum) {

        onSpectrum(
            std::vector<float>(
                32,
                0.0f
            )
        );
    }
}

// ============================================================
// SEEK
// ============================================================

void SimplePlayer::seek(
    double r
)
{
    r =
        std::clamp(
            r,
            0.0,
            1.0
        );

    gint64 d = 0;

    if (
        !gst_element_query_duration(
            impl->playbin,
            GST_FORMAT_TIME,
            &d
        )
        ||
        d <= 0
    ) {

        return;
    }

    gst_element_seek_simple(
        impl->playbin,

        GST_FORMAT_TIME,

        (GstSeekFlags)(
            GST_SEEK_FLAG_FLUSH |
            GST_SEEK_FLAG_KEY_UNIT
        ),

        (gint64)(
            d * r
        )
    );
}

// ============================================================
// VOLUME
// ============================================================

void SimplePlayer::setVolume(
    float v
)
{
    v =
        std::clamp(
            v,
            0.0f,
            1.0f
        );

    g_object_set(
        impl->playbin,
        "volume",
        (gdouble)v,
        nullptr
    );
}

// ============================================================
// POSITION
// ============================================================

double SimplePlayer::getPosition()
{
    gint64 p = 0;

    gst_element_query_position(
        impl->playbin,
        GST_FORMAT_TIME,
        &p
    );

    return p / 1e9;
}

// ============================================================
// DURATION
// ============================================================

double SimplePlayer::getDuration()
{
    gint64 d = 0;

    gst_element_query_duration(
        impl->playbin,
        GST_FORMAT_TIME,
        &d
    );

    return d / 1e9;
}

// ============================================================
// PLAYING
// ============================================================

bool SimplePlayer::isPlaying()
{
    return impl->playing;
}


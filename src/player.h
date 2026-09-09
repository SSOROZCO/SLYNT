#pragma once
#include <string>
#include <functional>
#include <cstdint>
#include <vector>
#include <gst/gst.h>

enum class OutputMode { Auto, Exclusive, Compatible };

struct SimplePlayer {
    struct PlayerImpl;
    PlayerImpl* impl = nullptr;

    std::string currentPath;
    std::string hwDevice = "";
    std::string currentDevice = "No DAC";
    OutputMode outputMode = OutputMode::Exclusive;
    int m_currentId = -1;
    int consecutiveErrors = 0;

    // fallback state
    bool isFallbackPCM = false;
    std::string originalDsdFormat = ""; // ej "DSD256"

    std::function<void()> onTrackFinished;
    std::function<void(OutputMode)> onOutputModeChanged;
    std::function<void(std::vector<float>)> onSpectrum;
    std::function<void(bool isFallback, std::string orig)> onFallbackChanged;

    SimplePlayer();
    ~SimplePlayer();

    void load(const std::string& path);
    void play();
    void pause();
    void stop();
    void seek(double ratio);
    void setVolume(float v);
    void setOutputMode(OutputMode mode);

    double getPosition();
    double getDuration();
    bool isPlaying();
    std::string getCurrentDevice() const { return currentDevice; }
    bool isBitPerfectFor(const std::string& path);
    GstElement* createSink(const std::string& path);
    void handleDeviceBusy();
    void handleDsdIncompatible();
    bool getIsFallback() const { return isFallbackPCM; }

    std::function<void()> onFinished;
    void setOnFinished(std::function<void()> cb){ onFinished = cb; }
};

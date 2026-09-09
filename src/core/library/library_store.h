#pragma once
#include <string>
#include <vector>
#include <functional>
#include <sqlite3.h>
#include <mutex>
#include <map>
#include <unordered_map>

struct AlbumData {
    int id;
    std::string title;
    std::string artist;
    std::string year;
    std::string cover_path;
    std::string root_path;
    int track_count;
};

struct TrackDataLite {
    std::string path;
    std::string title;
    std::string artist;
    std::string album;
    std::string year;
    std::string duration;
};

class LibraryStore {
public:
    LibraryStore();
    ~LibraryStore();

    // Base de datos
    bool Open();

    // Consultas
    std::vector<AlbumData> GetAlbums(const std::string& filter = "");
    std::vector<TrackDataLite> GetTracksForAlbum(int albumId);
    bool IsAvailable(const std::string& path);

    // Escaneo
    void ScanFolderAsync(const std::string& folder,
        std::function<void(int, std::string, int)> on_progress,
        std::function<void()> on_finished);

    // Delete methods (solo DB, no borra archivos del disco)
    bool DeleteAlbum(int albumId);
    bool DeleteAll();

    // ============================================================
    // PORTADAS - Estilo Strawberry
    // ============================================================

    // Cache de portadas en memoria (últimas 100)
    std::string GetCachedThumbnail(const std::string& coverPath);

private:
    sqlite3* db = nullptr;
    std::mutex mtx;
    std::string dbPath;

    // Cache de thumbnails en memoria
    std::unordered_map<std::string, std::string> thumbnailCache; // path -> thumbnail_path
    std::mutex cacheMutex;

    // Métodos privados
    void ensureTables();
    void scanSync(const std::string& folder, std::function<void(int, std::string, int)> on_progress);

    // ============================================================
    // MÉTODOS DE PORTADAS
    // ============================================================

    // Busca archivos de portada en la carpeta (cover.jpg, folder.jpg, etc.)
    std::string findCover(const std::string& folder);

    // Extrae portada embebida de FLAC o MP3
    std::string extractEmbeddedCover(const std::string& audioFile);

    // Genera una miniatura 256x256 en ~/.cache/slynt/thumbs/
    std::string generateThumbnail(const std::string& sourcePath);

    // Devuelve la ruta de la miniatura (la genera si no existe)
    std::string getThumbnailPath(const std::string& coverPath);
};

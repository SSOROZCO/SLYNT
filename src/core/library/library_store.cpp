#include "library_store.h"
#include <filesystem>
#include <algorithm>
#include <thread>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <taglib/fileref.h>
#include <taglib/tag.h>
#include <taglib/flacfile.h>
#include <taglib/mpegfile.h>
#include <taglib/id3v2tag.h>
#include <taglib/attachedpictureframe.h>

namespace fs = std::filesystem;

// ============================================================
// CONSTRUCTOR / DESTRUCTOR
// ============================================================
LibraryStore::LibraryStore() {
    const char* home = getenv("HOME");
    dbPath = std::string(home ? home : ".") + "/.local/share/slynt/library.db";
    fs::create_directories(fs::path(dbPath).parent_path());

    // Crear directorio de caché para thumbnails
    std::string cacheDir = std::string(home ? home : ".") + "/.cache/slynt/thumbs/";
    fs::create_directories(cacheDir);
}

LibraryStore::~LibraryStore(){
    if(db) sqlite3_close(db);
}

// ============================================================
// BASE DE DATOS
// ============================================================
bool LibraryStore::Open(){
    if(sqlite3_open(dbPath.c_str(), &db)!=SQLITE_OK) return false;
    ensureTables();
    return true;
}

void LibraryStore::ensureTables(){
    const char* sql = R"(
    CREATE TABLE IF NOT EXISTS albums(
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        title TEXT, artist TEXT, year TEXT,
        cover_path TEXT, root_path TEXT, track_count INTEGER
    );
    CREATE TABLE IF NOT EXISTS tracks(
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        album_id INTEGER,
        path TEXT, title TEXT, artist TEXT, album TEXT, year TEXT, duration TEXT,
        FOREIGN KEY(album_id) REFERENCES albums(id)
    );
    )";
    char* err=nullptr;
    sqlite3_exec(db, sql, nullptr, nullptr, &err);
}

// ============================================================
// FIND COVER - Busca archivos de portada en la carpeta
// ============================================================
std::string LibraryStore::findCover(const std::string& folder){
    const std::vector<std::string> names = {
        "cover.jpg", "Cover.jpg", "COVER.JPG",
        "folder.jpg", "Folder.jpg", "FOLDER.JPG",
        "front.jpg", "Front.jpg", "FRONT.JPG",
        "album.jpg", "Album.jpg", "ALBUM.JPG",
        "art.jpg", "Art.jpg", "ART.JPG",
        "cover.png", "Cover.png", "COVER.PNG",
        "folder.png", "Folder.png", "FOLDER.PNG"
    };

    for(auto &n: names){
        auto p = fs::path(folder) / n;
        if(fs::exists(p) && fs::file_size(p) > 5000) {
            return p.string();
        }
    }

    // Si no hay, buscar cualquier .jpg/.png >5KB
    try {
        for(auto &e: fs::directory_iterator(folder)) {
            if(!e.is_regular_file()) continue;
            auto ext = e.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            if((ext==".jpg" || ext==".jpeg" || ext==".png") && fs::file_size(e.path()) > 5000) {
                return e.path().string();
            }
        }
    } catch(...) {}

    return "";
}

// ============================================================
// EXTRAER PORTADA EMBEBIDA DE FLAC/MP3
// ============================================================
std::string LibraryStore::extractEmbeddedCover(const std::string& audioFile) {
    try {
        TagLib::FileRef fr(audioFile.c_str());
        if(fr.isNull()) return "";

        const char* home = getenv("HOME");
        std::string cacheDir = std::string(home ? home : ".") + "/.cache/slynt/thumbs/";
        std::string hash = std::to_string(std::hash<std::string>{}(audioFile));
        std::string tempPath = cacheDir + hash + "_embedded.jpg";

        // FLAC
        if(auto* flac = dynamic_cast<TagLib::FLAC::File*>(fr.file())) {
            // ✅ CORREGIDO: usar el tipo correcto
            TagLib::List<TagLib::FLAC::Picture*> pictures = flac->pictureList();
            if(!pictures.isEmpty()) {
                auto* pic = pictures.front();
                if(pic && !pic->data().isEmpty()) {
                    std::ofstream out(tempPath, std::ios::binary);
                    out.write(pic->data().data(), pic->data().size());
                    out.close();
                    if(fs::exists(tempPath) && fs::file_size(tempPath) > 0) {
                        return tempPath;
                    }
                }
            }
        }

        // MP3 (ID3v2)
        if(auto* mpeg = dynamic_cast<TagLib::MPEG::File*>(fr.file())) {
            auto* tag = mpeg->ID3v2Tag();
            if(tag) {
                auto frames = tag->frameList("APIC");
                if(!frames.isEmpty()) {
                    auto* frame = dynamic_cast<TagLib::ID3v2::AttachedPictureFrame*>(frames.front());
                    if(frame && !frame->picture().isEmpty()) {
                        std::ofstream out(tempPath, std::ios::binary);
                        out.write(frame->picture().data(), frame->picture().size());
                        out.close();
                        if(fs::exists(tempPath) && fs::file_size(tempPath) > 0) {
                            return tempPath;
                        }
                    }
                }
            }
        }
    } catch(...) {}
    return "";
}

// ============================================================
// GENERAR MINIATURA 256x256 (usando ImageMagick)
// ============================================================
std::string LibraryStore::generateThumbnail(const std::string& sourcePath) {
    if(sourcePath.empty() || !fs::exists(sourcePath)) return "";

    const char* home = getenv("HOME");
    std::string cacheDir = std::string(home ? home : ".") + "/.cache/slynt/thumbs/";
    std::string hash = std::to_string(std::hash<std::string>{}(sourcePath));
    std::string thumbPath = cacheDir + hash + ".jpg";

    // Si ya existe, devolverlo
    if(fs::exists(thumbPath) && fs::file_size(thumbPath) > 0) {
        return thumbPath;
    }

    // Intentar generar con ImageMagick
    std::string cmd = "convert \"" + sourcePath + "\" -resize 256x256^ -gravity center -extent 256x256 -quality 85 \"" + thumbPath + "\" 2>/dev/null";
    int result = system(cmd.c_str());

    // Si falló, copiar original (Slint lo escalará en GPU)
    if(result != 0 || !fs::exists(thumbPath)) {
        try {
            fs::copy(sourcePath, thumbPath, fs::copy_options::overwrite_existing);
        } catch(...) {
            return sourcePath; // Si todo falla, devolver original
        }
    }

    return thumbPath;
}

// ============================================================
// GET THUMBNAIL CON CACHE EN MEMORIA (últimas 100)
// ============================================================
std::string LibraryStore::GetCachedThumbnail(const std::string& coverPath) {
    if(coverPath.empty()) return "";

    std::lock_guard<std::mutex> lock(cacheMutex);

    // Verificar cache en memoria
    auto it = thumbnailCache.find(coverPath);
    if(it != thumbnailCache.end()) {
        return it->second;
    }

    // Generar miniatura
    std::string thumb = generateThumbnail(coverPath);
    if(!thumb.empty()) {
        // Mantener cache de 100 imágenes (LRU simple)
        if(thumbnailCache.size() > 100) {
            thumbnailCache.erase(thumbnailCache.begin());
        }
        thumbnailCache[coverPath] = thumb;
    }

    return thumb;
}

// ============================================================
// SCAN SYNC - Optimizado con portadas
// ============================================================
void LibraryStore::scanSync(const std::string& folder, std::function<void(int, std::string, int)> on_progress){
    std::lock_guard<std::mutex> lock(mtx);
    sqlite3_exec(db, "DELETE FROM tracks; DELETE FROM albums;", nullptr,nullptr,nullptr);

    std::vector<std::string> allFiles;
    for(auto &p: fs::recursive_directory_iterator(folder, fs::directory_options::skip_permission_denied)){
        if(!p.is_regular_file()) continue;
        auto ext = p.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if(ext==".flac"||ext==".mp3"||ext==".wav"||ext==".m4a"||ext==".ogg"||ext==".opus"||ext==".dsf"||ext==".dff")
            allFiles.push_back(p.path().string());
    }

    std::map<std::string, std::vector<std::string>> byFolder;
    for(auto &f: allFiles) byFolder[fs::path(f).parent_path().string()].push_back(f);

    int total = byFolder.size();
    int idx=0;
    for(auto &kv: byFolder){
        std::string title = fs::path(kv.first).filename().string();
        std::string artist = "Unknown";
        std::string year="";

        // ============================================================
        // BUSCAR PORTADA: archivo > embebida
        // ============================================================
        std::string cover = findCover(kv.first);

        // Si no hay archivo, buscar portada embebida en el primer archivo
        if(cover.empty() && !kv.second.empty()) {
            cover = extractEmbeddedCover(kv.second[0]);
        }

        // Generar miniatura (solo si hay portada)
        if(!cover.empty()) {
            cover = generateThumbnail(cover);
        }

        if(!kv.second.empty()){
            TagLib::FileRef fr(kv.second[0].c_str());
            if(!fr.isNull() && fr.tag()){
                if(!fr.tag()->album().isEmpty()) title = fr.tag()->album().to8Bit(true);
                if(!fr.tag()->artist().isEmpty()) artist = fr.tag()->artist().to8Bit(true);
                if(fr.tag()->year()>0) year = std::to_string(fr.tag()->year());
            }
        }

        sqlite3_stmt* stmt;
        sqlite3_prepare_v2(db, "INSERT INTO albums(title,artist,year,cover_path,root_path,track_count) VALUES(?,?,?,?,?,?)", -1, &stmt, nullptr);
        sqlite3_bind_text(stmt,1,title.c_str(),-1,SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt,2,artist.c_str(),-1,SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt,3,year.c_str(),-1,SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt,4,cover.c_str(),-1,SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt,5,kv.first.c_str(),-1,SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt,6,kv.second.size());
        sqlite3_step(stmt);
        int albumId = sqlite3_last_insert_rowid(db);
        sqlite3_finalize(stmt);

        for(auto &fp: kv.second){
            std::string tTitle = fs::path(fp).stem().string();
            std::string tArtist = artist;
            std::string tAlbum = title;
            std::string tYear = year;
            std::string dur="--:--";
            TagLib::FileRef fr(fp.c_str());
            if(!fr.isNull()){
                if(fr.tag()){
                    if(!fr.tag()->title().isEmpty()) tTitle = fr.tag()->title().to8Bit(true);
                    if(!fr.tag()->artist().isEmpty()) tArtist = fr.tag()->artist().to8Bit(true);
                }
                if(fr.audioProperties()){
                    int secs = fr.audioProperties()->length();
                    char buf[16]; snprintf(buf,sizeof(buf),"%d:%02d",secs/60,secs%60); dur=buf;
                }
            }
            sqlite3_prepare_v2(db, "INSERT INTO tracks(album_id,path,title,artist,album,year,duration) VALUES(?,?,?,?,?,?,?)", -1, &stmt, nullptr);
            sqlite3_bind_int(stmt,1,albumId);
            sqlite3_bind_text(stmt,2,fp.c_str(),-1,SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt,3,tTitle.c_str(),-1,SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt,4,tArtist.c_str(),-1,SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt,5,tAlbum.c_str(),-1,SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt,6,tYear.c_str(),-1,SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt,7,dur.c_str(),-1,SQLITE_TRANSIENT);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
        idx++;
        int pct = total>0 ? idx*100/total : 100;
        if(on_progress) on_progress(pct, kv.first, idx);
    }
}

// ============================================================
// SCAN ASYNC
// ============================================================
void LibraryStore::ScanFolderAsync(const std::string& folder,
    std::function<void(int, std::string, int)> on_progress,
    std::function<void()> on_finished){
    std::thread([this, folder, on_progress, on_finished]{
        scanSync(folder, on_progress);
        if(on_finished) on_finished();
    }).detach();
}

// ============================================================
// GET ALBUMS
// ============================================================
std::vector<AlbumData> LibraryStore::GetAlbums(const std::string& filter){
    std::lock_guard<std::mutex> lock(mtx);
    std::vector<AlbumData> out;
    std::string sql = "SELECT id,title,artist,year,cover_path,root_path,track_count FROM albums";
    if(!filter.empty()) sql += " WHERE title LIKE '%" + filter + "%' OR artist LIKE '%" + filter + "%'";
    sql += " ORDER BY artist, title";
    sqlite3_stmt* stmt;
    if(sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr)!=SQLITE_OK) return out;
    while(sqlite3_step(stmt)==SQLITE_ROW){
        AlbumData a;
        a.id = sqlite3_column_int(stmt,0);
        a.title = (const char*)sqlite3_column_text(stmt,1) ? (const char*)sqlite3_column_text(stmt,1) : "";
        a.artist = (const char*)sqlite3_column_text(stmt,2) ? (const char*)sqlite3_column_text(stmt,2) : "";
        a.year = (const char*)sqlite3_column_text(stmt,3) ? (const char*)sqlite3_column_text(stmt,3) : "";
        a.cover_path = (const char*)sqlite3_column_text(stmt,4) ? (const char*)sqlite3_column_text(stmt,4) : "";
        a.root_path = (const char*)sqlite3_column_text(stmt,5) ? (const char*)sqlite3_column_text(stmt,5) : "";
        a.track_count = sqlite3_column_int(stmt,6);
        out.push_back(a);
    }
    sqlite3_finalize(stmt);
    return out;
}

// ============================================================
// GET TRACKS FOR ALBUM
// ============================================================
std::vector<TrackDataLite> LibraryStore::GetTracksForAlbum(int albumId){
    std::lock_guard<std::mutex> lock(mtx);
    std::vector<TrackDataLite> out;
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db, "SELECT path,title,artist,album,year,duration FROM tracks WHERE album_id=? ORDER BY path", -1, &stmt, nullptr);
    sqlite3_bind_int(stmt,1,albumId);
    while(sqlite3_step(stmt)==SQLITE_ROW){
        TrackDataLite t;
        t.path = (const char*)sqlite3_column_text(stmt,0) ? (const char*)sqlite3_column_text(stmt,0) : "";
        t.title = (const char*)sqlite3_column_text(stmt,1) ? (const char*)sqlite3_column_text(stmt,1) : "";
        t.artist = (const char*)sqlite3_column_text(stmt,2) ? (const char*)sqlite3_column_text(stmt,2) : "";
        t.album = (const char*)sqlite3_column_text(stmt,3) ? (const char*)sqlite3_column_text(stmt,3) : "";
        t.year = (const char*)sqlite3_column_text(stmt,4) ? (const char*)sqlite3_column_text(stmt,4) : "";
        t.duration = (const char*)sqlite3_column_text(stmt,5) ? (const char*)sqlite3_column_text(stmt,5) : "";
        out.push_back(t);
    }
    sqlite3_finalize(stmt);
    return out;
}

// ============================================================
// IS AVAILABLE
// ============================================================
bool LibraryStore::IsAvailable(const std::string& path){
    return std::filesystem::exists(path);
}

// ============================================================
// MÉTODOS DELETE - SOLO BORRAN DE LA DB, NO DEL DISCO
// ============================================================
bool LibraryStore::DeleteAlbum(int albumId) {
    std::lock_guard<std::mutex> lock(mtx);
    sqlite3_exec(db, "BEGIN TRANSACTION;", nullptr, nullptr, nullptr);
    sqlite3_stmt* stmt;

    sqlite3_prepare_v2(db, "DELETE FROM tracks WHERE album_id=?;", -1, &stmt, nullptr);
    sqlite3_bind_int(stmt, 1, albumId);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    sqlite3_prepare_v2(db, "DELETE FROM albums WHERE id=?;", -1, &stmt, nullptr);
    sqlite3_bind_int(stmt, 1, albumId);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    sqlite3_exec(db, "COMMIT;", nullptr, nullptr, nullptr);
    return true;
}

bool LibraryStore::DeleteAll() {
    std::lock_guard<std::mutex> lock(mtx);
    sqlite3_exec(db, "DELETE FROM tracks;", nullptr, nullptr, nullptr);
    sqlite3_exec(db, "DELETE FROM albums;", nullptr, nullptr, nullptr);
    sqlite3_exec(db, "VACUUM;", nullptr, nullptr, nullptr);
    return true;
}

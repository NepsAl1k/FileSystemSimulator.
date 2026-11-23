#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <sstream>
#include <cstring>
#include <cstdint>
#include <algorithm>
#include <iomanip>

using namespace std;

static constexpr uint32_t BLOCK_SIZE = 1024;
static constexpr uint32_t TOTAL_BLOCKS = 1024;
static constexpr uint32_t INODE_COUNT = 64;
static constexpr uint32_t DIRECT_COUNT = 12;
static constexpr uint32_t MAGIC = 0x46535953; // FSYS
static constexpr const char* IMAGE = "filesystem.img";

enum : uint8_t { FREE_NODE = 0, FILE_NODE = 1, DIR_NODE = 2 };

struct Superblock {
    uint32_t magic, version, blockSize, totalBlocks, inodeCount;
    uint32_t blockBitmapStart, inodeBitmapStart, inodeTableStart, inodeTableBlocks, dataStart;
};

struct Inode {
    uint8_t type;
    uint8_t links;
    uint16_t reserved;
    uint32_t size;
    uint32_t direct[DIRECT_COUNT];
    uint32_t indirect;
    uint32_t created;
    uint32_t modified;
    uint8_t padding[60];
};
static_assert(sizeof(Inode) == 128, "Inode must be 128 bytes");

struct DirEntry { uint32_t inode; char name[60]; };
static_assert(sizeof(DirEntry) == 64, "DirEntry must be 64 bytes");

class FileSystem {
    fstream disk;
    Superblock sb{};

    uint64_t blockOffset(uint32_t b) const { return uint64_t(b) * BLOCK_SIZE; }
    bool readAt(uint64_t pos, char* data, size_t n) {
        disk.clear(); disk.seekg(static_cast<streamoff>(pos));
        disk.read(data, static_cast<streamsize>(n));
        return bool(disk);
    }
    bool writeAt(uint64_t pos, const char* data, size_t n) {
        disk.clear(); disk.seekp(static_cast<streamoff>(pos));
        disk.write(data, static_cast<streamsize>(n)); disk.flush();
        return bool(disk);
    }
    bool bitGet(uint32_t block, uint32_t index) {
        unsigned char byte = 0;
        readAt(blockOffset(block) + index / 8, reinterpret_cast<char*>(&byte), 1);
        return (byte >> (index % 8)) & 1;
    }
    void bitSet(uint32_t block, uint32_t index, bool value) {
        unsigned char byte = 0;
        readAt(blockOffset(block) + index / 8, reinterpret_cast<char*>(&byte), 1);
        if (value) byte |= static_cast<unsigned char>(1u << (index % 8));
        else byte &= static_cast<unsigned char>(~(1u << (index % 8)));
        writeAt(blockOffset(block) + index / 8, reinterpret_cast<char*>(&byte), 1);
    }
    bool inodeUsed(uint32_t i) { return bitGet(sb.inodeBitmapStart, i); }
    void setInodeUsed(uint32_t i, bool v) { bitSet(sb.inodeBitmapStart, i, v); }
    bool blockUsed(uint32_t b) { return bitGet(sb.blockBitmapStart, b); }
    void setBlockUsed(uint32_t b, bool v) { bitSet(sb.blockBitmapStart, b, v); }

    bool readInode(uint32_t id, Inode& n) {
        if (id >= sb.inodeCount) return false;
        return readAt(blockOffset(sb.inodeTableStart) + uint64_t(id) * sizeof(Inode), reinterpret_cast<char*>(&n), sizeof(n));
    }
    bool writeInode(uint32_t id, const Inode& n) {
        if (id >= sb.inodeCount) return false;
        return writeAt(blockOffset(sb.inodeTableStart) + uint64_t(id) * sizeof(Inode), reinterpret_cast<const char*>(&n), sizeof(n));
    }
    int allocBlock() {
        for (uint32_t b = sb.dataStart; b < sb.totalBlocks; ++b) {
            if (!blockUsed(b)) {
                setBlockUsed(b, true);
                char zero[BLOCK_SIZE]{}; writeAt(blockOffset(b), zero, BLOCK_SIZE);
                return static_cast<int>(b);
            }
        }
        return -1;
    }
    void freeBlock(uint32_t b) { if (b >= sb.dataStart && b < sb.totalBlocks) setBlockUsed(b, false); }
    int allocInode(uint8_t type) {
        for (uint32_t i = 1; i < sb.inodeCount; ++i) {
            if (!inodeUsed(i)) {
                setInodeUsed(i, true);
                Inode n{}; n.type = type; n.links = 1; n.created = n.modified = now();
                writeInode(i, n); return static_cast<int>(i);
            }
        }
        return -1;
    }
    static uint32_t now() { return static_cast<uint32_t>(time(nullptr)); }

    vector<uint32_t> dataBlocks(const Inode& n) {
        vector<uint32_t> out;
        for (uint32_t b : n.direct) if (b) out.push_back(b);
        if (n.indirect) {
            uint32_t table[BLOCK_SIZE / sizeof(uint32_t)]{};
            readAt(blockOffset(n.indirect), reinterpret_cast<char*>(table), BLOCK_SIZE);
            for (uint32_t b : table) if (b) out.push_back(b);
        }
        return out;
    }
    bool getFileBlock(const Inode& n, uint32_t index, uint32_t& result) {
        if (index < DIRECT_COUNT) { result = n.direct[index]; return result != 0; }
        if (!n.indirect) return false;
        uint32_t table[BLOCK_SIZE / sizeof(uint32_t)]{};
        if (!readAt(blockOffset(n.indirect), reinterpret_cast<char*>(table), BLOCK_SIZE)) return false;
        uint32_t k = index - DIRECT_COUNT;
        if (k >= BLOCK_SIZE / sizeof(uint32_t)) return false;
        result = table[k]; return result != 0;
    }
    bool setFileBlock(Inode& n, uint32_t index, uint32_t block) {
        if (index < DIRECT_COUNT) { n.direct[index] = block; return true; }
        uint32_t k = index - DIRECT_COUNT;
        if (k >= BLOCK_SIZE / sizeof(uint32_t)) return false;
        if (!n.indirect) {
            int t = allocBlock(); if (t < 0) return false;
            n.indirect = static_cast<uint32_t>(t);
        }
        uint32_t table[BLOCK_SIZE / sizeof(uint32_t)]{};
        readAt(blockOffset(n.indirect), reinterpret_cast<char*>(table), BLOCK_SIZE);
        table[k] = block;
        return writeAt(blockOffset(n.indirect), reinterpret_cast<const char*>(table), BLOCK_SIZE);
    }
    void releaseData(Inode& n) {
        for (uint32_t b : n.direct) if (b) freeBlock(b);
        if (n.indirect) {
            uint32_t table[BLOCK_SIZE / sizeof(uint32_t)]{};
            readAt(blockOffset(n.indirect), reinterpret_cast<char*>(table), BLOCK_SIZE);
            for (uint32_t b : table) if (b) freeBlock(b);
            freeBlock(n.indirect);
        }
        memset(n.direct, 0, sizeof(n.direct)); n.indirect = 0; n.size = 0;
    }
    bool readBytes(const Inode& n, string& out) {
        out.clear(); out.resize(n.size);
        size_t done = 0;
        while (done < n.size) {
            uint32_t b;
            if (!getFileBlock(n, static_cast<uint32_t>(done / BLOCK_SIZE), b)) return false;
            size_t chunk = min<size_t>(BLOCK_SIZE, n.size - done);
            if (!readAt(blockOffset(b), &out[done], chunk)) return false;
            done += chunk;
        }
        return true;
    }
    bool writeBytes(Inode& n, const string& data) {
        const uint32_t maxBlocks = DIRECT_COUNT + BLOCK_SIZE / sizeof(uint32_t);
        if (data.size() > uint64_t(maxBlocks) * BLOCK_SIZE) return false;
        releaseData(n);
        size_t done = 0; uint32_t index = 0;
        while (done < data.size()) {
            int b = allocBlock(); if (b < 0) { releaseData(n); return false; }
            if (!setFileBlock(n, index++, static_cast<uint32_t>(b))) { freeBlock(static_cast<uint32_t>(b)); releaseData(n); return false; }
            size_t chunk = min<size_t>(BLOCK_SIZE, data.size() - done);
            if (!writeAt(blockOffset(static_cast<uint32_t>(b)), data.data() + done, chunk)) return false;
            done += chunk;
        }
        n.size = static_cast<uint32_t>(data.size()); n.modified = now();
        return true;
    }
    bool readDir(uint32_t id, vector<DirEntry>& entries) {
        Inode n{}; if (!readInode(id, n) || n.type != DIR_NODE) return false;
        string raw; if (!readBytes(n, raw)) return false;
        entries.clear();
        for (size_t p = 0; p + sizeof(DirEntry) <= raw.size(); p += sizeof(DirEntry)) {
            DirEntry e{}; memcpy(&e, raw.data() + p, sizeof(e));
            if (e.inode != UINT32_MAX) entries.push_back(e);
        }
        return true;
    }
    bool writeDir(uint32_t id, const vector<DirEntry>& entries) {
        Inode n{}; if (!readInode(id, n) || n.type != DIR_NODE) return false;
        string raw(entries.size() * sizeof(DirEntry), '\0');
        for (size_t i = 0; i < entries.size(); ++i) memcpy(&raw[i * sizeof(DirEntry)], &entries[i], sizeof(DirEntry));
        return writeBytes(n, raw) && writeInode(id, n);
    }
    static vector<string> splitPath(const string& path) {
        vector<string> parts; string part; stringstream ss(path);
        while (getline(ss, part, '/')) if (!part.empty()) parts.push_back(part);
        return parts;
    }
    int findEntry(uint32_t dir, const string& name) {
        vector<DirEntry> es; if (!readDir(dir, es)) return -1;
        for (const auto& e : es) if (name == e.name) return static_cast<int>(e.inode);
        return -1;
    }
    int resolve(const string& path) {
        uint32_t current = 0;
        vector<string> parts = splitPath(path);
        for (size_t i = 0; i < parts.size(); ++i) {
            const string& part = parts[i];
            if (part == ".") continue;
            if (part == "..") {
                int parent = findEntry(current, "..");
                if (parent < 0) return -1;
                current = static_cast<uint32_t>(parent);
                continue;
            }
            Inode currentNode{};
            if (!readInode(current, currentNode) || currentNode.type != DIR_NODE) return -1;
            int next = findEntry(current, part);
            if (next < 0) return -1;
            Inode nextNode{};
            if (!readInode(static_cast<uint32_t>(next), nextNode)) return -1;
            if (i + 1 < parts.size() && nextNode.type != DIR_NODE) return -1;
            current = static_cast<uint32_t>(next);
        }
        return static_cast<int>(current);
    }
    bool parentAndName(const string& path, uint32_t& parent, string& name) {
        auto parts = splitPath(path); if (parts.empty()) return false;
        name = parts.back(); if (name == "." || name == ".." || name.size() >= sizeof(DirEntry::name)) return false;
        parts.pop_back(); parent = 0;
        for (const string& p : parts) {
            if (p == ".") continue;
            if (p == "..") { int q = findEntry(parent, ".."); if (q < 0) return false; parent = static_cast<uint32_t>(q); }
            else { int q = findEntry(parent, p); if (q < 0) return false; Inode n{}; readInode(q, n); if (n.type != DIR_NODE) return false; parent = static_cast<uint32_t>(q); }
        }
        return true;
    }
    bool addEntry(uint32_t parent, const string& name, uint32_t id) {
        vector<DirEntry> es; if (!readDir(parent, es)) return false;
        for (auto& e : es) if (name == e.name) return false;
        DirEntry e{}; e.inode = id; strncpy(e.name, name.c_str(), sizeof(e.name)-1); es.push_back(e);
        return writeDir(parent, es);
    }
    bool removeEntry(uint32_t parent, const string& name) {
        vector<DirEntry> es; if (!readDir(parent, es)) return false;
        auto it = find_if(es.begin(), es.end(), [&](const DirEntry& e){ return name == e.name; });
        if (it == es.end()) return false; es.erase(it); return writeDir(parent, es);
    }
    void destroyInode(uint32_t id, Inode& n) { releaseData(n); writeInode(id, Inode{}); setInodeUsed(id, false); }

public:
    ~FileSystem() { if (disk.is_open()) disk.close(); }
    bool format() {
        if (disk.is_open()) disk.close();
        { ofstream out(IMAGE, ios::binary | ios::trunc); if (!out) return false; char z[BLOCK_SIZE]{}; for (uint32_t i=0;i<TOTAL_BLOCKS;++i) out.write(z,BLOCK_SIZE); }
        disk.open(IMAGE, ios::binary | ios::in | ios::out); if (!disk) return false;
        sb = {MAGIC,1,BLOCK_SIZE,TOTAL_BLOCKS,INODE_COUNT,1,2,3,8,11};
        writeAt(0, reinterpret_cast<const char*>(&sb), sizeof(sb));
        unsigned char bm[BLOCK_SIZE]{}; for (uint32_t i=0;i<sb.dataStart;++i) bm[i/8] |= static_cast<unsigned char>(1u<<(i%8));
        writeAt(blockOffset(sb.blockBitmapStart), reinterpret_cast<char*>(bm), BLOCK_SIZE);
        unsigned char im[BLOCK_SIZE]{}; im[0] = 1; writeAt(blockOffset(sb.inodeBitmapStart), reinterpret_cast<char*>(im), BLOCK_SIZE);
        Inode root{}; root.type=DIR_NODE; root.links=1; root.created=root.modified=now();
        int rb=allocBlock(); if (rb<0) return false; root.direct[0]=static_cast<uint32_t>(rb);
        DirEntry entries[2]{}; entries[0].inode=0; strcpy(entries[0].name,"."); entries[1].inode=0; strcpy(entries[1].name,"..");
        writeAt(blockOffset(static_cast<uint32_t>(rb)), reinterpret_cast<char*>(entries), sizeof(entries)); root.size=sizeof(entries);
        writeInode(0,root); return true;
    }
    bool mount() {
        if (disk.is_open()) disk.close();
        disk.open(IMAGE, ios::binary | ios::in | ios::out); if (!disk) return false;
        if (!readAt(0, reinterpret_cast<char*>(&sb), sizeof(sb))) return false;
        return sb.magic==MAGIC && sb.blockSize==BLOCK_SIZE && sb.totalBlocks==TOTAL_BLOCKS && sb.dataStart==11;
    }
    void info() {
        uint32_t used=0, usedIn=0; for(uint32_t b=0;b<sb.totalBlocks;++b) if(blockUsed(b)) ++used;
        for(uint32_t i=0;i<sb.inodeCount;++i) if(inodeUsed(i)) ++usedIn;
        cout << "Образ: "<<IMAGE<<"\nБлок: "<<sb.blockSize<<" байт\nВсего блоков: "<<sb.totalBlocks<<"\nЗанято блоков: "<<used<<"\nСвободно блоков: "<<(sb.totalBlocks-used)<<"\nInode: "<<usedIn<<"/"<<sb.inodeCount<<"\nМаксимальный файл: "<<(DIRECT_COUNT+BLOCK_SIZE/4)*BLOCK_SIZE<<" байт (прямые + один косвенный уровень)\n";
    }
    bool create(const string& path, bool directory=false) {
        uint32_t parent; string name; if(!parentAndName(path,parent,name)) { cout<<"Неверный путь.\n"; return false; }
        if(findEntry(parent,name)>=0) { cout<<"Объект уже существует.\n"; return false; }
        int id=allocInode(directory?DIR_NODE:FILE_NODE); if(id<0){cout<<"Нет свободных inode.\n";return false;}
        if(directory) {
            Inode n{}; readInode(id,n); int b=allocBlock(); if(b<0){destroyInode(id,n);return false;}
            n.direct[0]=static_cast<uint32_t>(b); DirEntry e[2]{}; e[0].inode=id; strcpy(e[0].name,"."); e[1].inode=parent; strcpy(e[1].name,"..");
            writeAt(blockOffset(static_cast<uint32_t>(b)),reinterpret_cast<char*>(e),sizeof(e)); n.size=sizeof(e); writeInode(id,n);
        }
        if(!addEntry(parent,name,static_cast<uint32_t>(id))) { Inode n{};readInode(id,n);destroyInode(id,n);cout<<"Не удалось добавить запись каталога.\n";return false; }
        cout<<(directory?"Каталог":"Файл")<<" создан: "<<path<<" (inode "<<id<<")\n"; return true;
    }
    void ls(const string& path) {
        int id=resolve(path.empty()?"/":path); if(id<0){cout<<"Путь не найден.\n";return;}
        Inode n{};readInode(id,n); if(n.type!=DIR_NODE){cout<<"Это не каталог.\n";return;}
        vector<DirEntry> es;readDir(id,es); cout<<"Содержимое "<<(path.empty()?"/":path)<<":\n";
        for(auto&e:es){if(string(e.name)=="."||string(e.name)=="..")continue;Inode x{};readInode(e.inode,x);cout<<(x.type==DIR_NODE?"[DIR] ":"[FILE] ")<<e.name<<"  inode="<<e.inode<<"  size="<<x.size<<"\n";}
    }
    void writeFile(const string& path,const string& content) {
        int id=resolve(path); if(id<0){cout<<"Файл не найден. Сначала create.\n";return;} Inode n{};readInode(id,n);if(n.type!=FILE_NODE){cout<<"Это не обычный файл.\n";return;}
        if(writeBytes(n,content)&&writeInode(id,n)) cout<<"Записано байт: "<<content.size()<<"\n"; else cout<<"Ошибка записи или недостаточно места.\n";
    }
    void readFile(const string& path) {
        int id=resolve(path);if(id<0){cout<<"Файл не найден.\n";return;}Inode n{};readInode(id,n);if(n.type!=FILE_NODE){cout<<"Это не обычный файл.\n";return;}string data;if(!readBytes(n,data)){cout<<"Ошибка чтения.\n";return;}cout<<"--- Содержимое ---\n"<<data<<"\n------------------\n";
    }
    void openFile(const string& path) { int id=resolve(path); if(id<0) cout<<"Не удалось открыть: объект не найден.\n"; else cout<<"Объект открыт (inode "<<id<<").\n"; }
    void stat(const string& path) {
        int id=resolve(path);if(id<0){cout<<"Объект не найден.\n";return;}Inode n{};readInode(id,n);
        cout<<"Путь: "<<path<<"\nInode: "<<id<<"\nТип: "<<(n.type==DIR_NODE?"каталог":"файл")<<"\nРазмер: "<<n.size<<" байт\nЖёстких ссылок: "<<unsigned(n.links)<<"\nБлоки данных: ";
        auto bs=dataBlocks(n);if(bs.empty())cout<<"нет";for(auto b:bs)cout<<b<<" ";cout<<"\n";
    }
    void linkFile(const string& oldPath,const string& newPath) {
        int id=resolve(oldPath);if(id<0){cout<<"Исходный файл не найден.\n";return;}Inode n{};readInode(id,n);if(n.type!=FILE_NODE){cout<<"Жёсткие ссылки на каталоги запрещены.\n";return;}if(n.links==255){cout<<"Достигнут предел ссылок.\n";return;}
        uint32_t p;string name;if(!parentAndName(newPath,p,name)||findEntry(p,name)>=0){cout<<"Неверный путь или имя уже занято.\n";return;}if(addEntry(p,name,static_cast<uint32_t>(id))){++n.links;writeInode(id,n);cout<<"Жёсткая ссылка создана.\n";}
    }
    void remove(const string& path) {
        uint32_t p;string name;if(!parentAndName(path,p,name)){cout<<"Неверный путь.\n";return;}int id=findEntry(p,name);if(id<0){cout<<"Объект не найден.\n";return;}Inode n{};readInode(id,n);
        if(n.type==DIR_NODE){vector<DirEntry> es;readDir(id,es);if(es.size()>2){cout<<"Каталог не пуст. Сначала удалите содержимое.\n";return;} }
        if(!removeEntry(p,name)){cout<<"Ошибка удаления записи.\n";return;}
        if(n.type==DIR_NODE||n.links<=1){destroyInode(static_cast<uint32_t>(id),n);}else{--n.links;writeInode(id,n);}cout<<"Удалено: "<<path<<"\n";
    }
    void check() {
        uint32_t bad=0;for(uint32_t i=0;i<sb.inodeCount;++i)if(inodeUsed(i)){Inode n{};if(!readInode(i,n)||n.type==FREE_NODE)++bad;}
        cout<<"Проверка образа завершена. Некорректных inode: "<<bad<<"\n";
    }
};

int main() {
    FileSystem fs;
    cout << "=== FILE SYSTEM SIMULATOR ===\n";
    cout << "Команды: format, mount, create, mkdir, open, write, read, delete, link, ls, stat, info, fsck, help, exit\n";
    string line;
    while (true) {
        cout << "fs> "; if(!getline(cin,line)) break;
        if(line.empty()) continue;
        string cmd; stringstream ss(line); ss>>cmd;
        if(cmd=="exit") break;
        if(cmd=="help") { cout<<"format | mount | create PATH | mkdir PATH | open PATH | write PATH TEXT | read PATH | delete PATH | link OLD NEW | ls [PATH] | stat PATH | info | fsck | exit\n"; continue; }
        if(cmd=="format") { if(fs.format()) cout<<"Файловая система создана и смонтирована.\n"; else cout<<"Ошибка форматирования.\n"; continue; }
        if(cmd=="mount") { if(fs.mount()) cout<<"Образ успешно смонтирован.\n"; else cout<<"Не удалось смонтировать образ. Сначала выполните format.\n"; continue; }
        if(cmd=="create"||cmd=="mkdir"||cmd=="open"||cmd=="read"||cmd=="delete"||cmd=="stat") {
            string path; ss>>path; if(path.empty()){cout<<"Укажите путь.\n";continue;}
            if(cmd=="create") fs.create(path); else if(cmd=="mkdir") fs.create(path,true); else if(cmd=="open") fs.openFile(path); else if(cmd=="read") fs.readFile(path); else if(cmd=="delete") fs.remove(path); else fs.stat(path); continue;
        }
        if(cmd=="write") { string path;ss>>path;string content;getline(ss,content);if(path.empty()){cout<<"Укажите путь.\n";continue;}if(!content.empty()&&content[0]==' ')content.erase(0,1);fs.writeFile(path,content);continue; }
        if(cmd=="link") { string a,b;ss>>a>>b;if(a.empty()||b.empty())cout<<"Использование: link OLD NEW\n";else fs.linkFile(a,b);continue; }
        if(cmd=="ls") { string p;ss>>p;fs.ls(p);continue; }
        if(cmd=="info") { fs.info();continue; }
        if(cmd=="fsck") { fs.check();continue; }
        cout<<"Неизвестная команда. Введите help.\n";
    }
    return 0;
}

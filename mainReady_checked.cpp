#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <sstream>
#include <cstring>
#include <cstdint>
#include <ctime>
#include <algorithm>
#include <iomanip>
#include <filesystem>

using namespace std;

// Educational filesystem image: 1 MiB, 1024-byte blocks.
static constexpr uint32_t BLOCK_SIZE = 1024;
static constexpr uint32_t TOTAL_BLOCKS = 1024;
static constexpr uint32_t INODE_COUNT = 64;
static constexpr uint32_t DIRECT_COUNT = 8;
static constexpr uint32_t MAGIC = 0x46535953; // "FSYS"
static constexpr const char* IMAGE = "filesystem.img";
static constexpr uint32_t JOURNAL_HEADER_BLOCK = 11;
static constexpr uint32_t JOURNAL_BACKUP_START = 12; // backup blocks 0..10
static constexpr uint32_t DATA_START = 23;
static constexpr uint32_t TYPE_FILE = 1;
static constexpr uint32_t TYPE_DIR = 2;

#pragma pack(push, 1)
struct Superblock {
    uint32_t magic, version, blockSize, totalBlocks, inodeCount;
    uint32_t blockBitmapStart, inodeBitmapStart, inodeTableStart;
    uint32_t journalStart, journalBlocks, dataStart, rootInode;
};
struct Inode {
    uint32_t used;
    uint32_t type;
    uint32_t size;
    uint32_t links;
    uint32_t direct[DIRECT_COUNT];
    uint32_t indirect;
    uint64_t created;
    uint8_t reserved[68];
};
struct DirEntry {
    uint32_t inode;
    uint8_t type;
    char name[59];
};
struct JournalHeader {
    uint32_t magic;
    uint32_t active;
    uint32_t checksum;
    uint32_t reserved[253];
};
#pragma pack(pop)
static_assert(sizeof(Inode) == 128, "Inode must be 128 bytes");
static_assert(sizeof(DirEntry) == 64, "DirEntry must be 64 bytes");
static_assert(sizeof(JournalHeader) == BLOCK_SIZE, "Journal header must fill one block");

class FileSystem {
    fstream disk;
    Superblock sb{};
    bool mounted = false;

    static uint64_t nowSeconds() { return static_cast<uint64_t>(time(nullptr)); }
    streamoff off(uint32_t block) const { return static_cast<streamoff>(block) * BLOCK_SIZE; }
    bool readBlock(uint32_t b, char* out) {
        if (b >= TOTAL_BLOCKS) return false;
        disk.clear(); disk.seekg(off(b)); disk.read(out, BLOCK_SIZE);
        return disk.good();
    }
    bool writeBlock(uint32_t b, const char* in) {
        if (b >= TOTAL_BLOCKS) return false;
        disk.clear(); disk.seekp(off(b)); disk.write(in, BLOCK_SIZE); disk.flush();
        return disk.good();
    }
    bool bitGet(uint32_t block, uint32_t bit) {
        char buf[BLOCK_SIZE]{}; if (!readBlock(block, buf)) return false;
        return (static_cast<unsigned char>(buf[bit / 8]) >> (bit % 8)) & 1;
    }
    bool bitSet(uint32_t block, uint32_t bit, bool value) {
        char buf[BLOCK_SIZE]{}; if (!readBlock(block, buf)) return false;
        unsigned char v = static_cast<unsigned char>(buf[bit / 8]);
        if (value) v |= static_cast<unsigned char>(1u << (bit % 8));
        else v &= static_cast<unsigned char>(~(1u << (bit % 8)));
        buf[bit / 8] = static_cast<char>(v); return writeBlock(block, buf);
    }
    bool loadInode(uint32_t id, Inode& n) {
        if (id >= INODE_COUNT) return false;
        disk.clear(); disk.seekg(off(sb.inodeTableStart) + static_cast<streamoff>(id) * sizeof(Inode));
        disk.read(reinterpret_cast<char*>(&n), sizeof(n)); return disk.good();
    }
    bool saveInode(uint32_t id, const Inode& n) {
        if (id >= INODE_COUNT) return false;
        disk.clear(); disk.seekp(off(sb.inodeTableStart) + static_cast<streamoff>(id) * sizeof(Inode));
        disk.write(reinterpret_cast<const char*>(&n), sizeof(n)); disk.flush(); return disk.good();
    }
    uint32_t allocBlock() {
        for (uint32_t b = sb.dataStart; b < TOTAL_BLOCKS; ++b) {
            if (!bitGet(sb.blockBitmapStart, b)) {
                bitSet(sb.blockBitmapStart, b, true);
                char zero[BLOCK_SIZE]{}; writeBlock(b, zero); return b;
            }
        }
        return 0;
    }
    void freeBlock(uint32_t b) { if (b >= sb.dataStart && b < TOTAL_BLOCKS) bitSet(sb.blockBitmapStart, b, false); }
    uint32_t allocInode(uint32_t type) {
        for (uint32_t i = 0; i < INODE_COUNT; ++i) if (!bitGet(sb.inodeBitmapStart, i)) {
            bitSet(sb.inodeBitmapStart, i, true);
            Inode n{}; n.used = 1; n.type = type; n.links = 1; n.created = nowSeconds();
            saveInode(i, n); return i;
        }
        return UINT32_MAX;
    }
    void freeInode(uint32_t id) {
        Inode n{}; if (loadInode(id, n)) {
            for (uint32_t b : n.direct) if (b) freeBlock(b);
            if (n.indirect) {
                uint32_t ptrs[BLOCK_SIZE / 4]{}; readBlock(n.indirect, reinterpret_cast<char*>(ptrs));
                for (uint32_t b : ptrs) if (b) freeBlock(b);
                freeBlock(n.indirect);
            }
        }
        Inode empty{}; saveInode(id, empty); bitSet(sb.inodeBitmapStart, id, false);
    }
    bool getDataBlock(const Inode& n, uint32_t index, uint32_t& b) {
        if (index < DIRECT_COUNT) { b = n.direct[index]; return b != 0; }
        if (!n.indirect || index >= DIRECT_COUNT + BLOCK_SIZE / 4) return false;
        uint32_t ptrs[BLOCK_SIZE / 4]{}; if (!readBlock(n.indirect, reinterpret_cast<char*>(ptrs))) return false;
        b = ptrs[index - DIRECT_COUNT]; return b != 0;
    }
    bool setDataBlock(Inode& n, uint32_t index, uint32_t b) {
        if (index < DIRECT_COUNT) { n.direct[index] = b; return true; }
        if (index >= DIRECT_COUNT + BLOCK_SIZE / 4) return false;
        if (!n.indirect) { n.indirect = allocBlock(); if (!n.indirect) return false; }
        uint32_t ptrs[BLOCK_SIZE / 4]{}; readBlock(n.indirect, reinterpret_cast<char*>(ptrs));
        ptrs[index - DIRECT_COUNT] = b;
        return writeBlock(n.indirect, reinterpret_cast<const char*>(ptrs));
    }
    bool readBytes(const Inode& n, string& out) {
        out.clear(); out.reserve(n.size);
        uint32_t blocks = (n.size + BLOCK_SIZE - 1) / BLOCK_SIZE;
        for (uint32_t i = 0; i < blocks; ++i) {
            uint32_t b; if (!getDataBlock(n, i, b)) return false;
            char buf[BLOCK_SIZE]{}; if (!readBlock(b, buf)) return false;
            uint32_t count = min(BLOCK_SIZE, n.size - static_cast<uint32_t>(out.size()));
            out.append(buf, count);
        }
        return true;
    }
    bool writeBytes(Inode& n, const string& data) {
        const uint32_t maxBytes = (DIRECT_COUNT + BLOCK_SIZE / 4) * BLOCK_SIZE;
        if (data.size() > maxBytes) { cout << "Ошибка: файл превышает максимальный размер " << maxBytes << " байт.\n"; return false; }
        uint32_t needed = (static_cast<uint32_t>(data.size()) + BLOCK_SIZE - 1) / BLOCK_SIZE;
        uint32_t old = (n.size + BLOCK_SIZE - 1) / BLOCK_SIZE;
        for (uint32_t i = 0; i < needed; ++i) {
            uint32_t b;
            if (!getDataBlock(n, i, b)) { b = allocBlock(); if (!b || !setDataBlock(n, i, b)) return false; }
            char buf[BLOCK_SIZE]{};
            size_t start = static_cast<size_t>(i) * BLOCK_SIZE;
            size_t count = min(static_cast<size_t>(BLOCK_SIZE), data.size() - start);
            if (count) memcpy(buf, data.data() + start, count);
            if (!writeBlock(b, buf)) return false;
        }
        for (uint32_t i = needed; i < old; ++i) {
            uint32_t b; if (getDataBlock(n, i, b)) freeBlock(b);
            if (i < DIRECT_COUNT) n.direct[i] = 0;
            else if (n.indirect) {
                uint32_t ptrs[BLOCK_SIZE / 4]{}; readBlock(n.indirect, reinterpret_cast<char*>(ptrs));
                ptrs[i - DIRECT_COUNT] = 0; writeBlock(n.indirect, reinterpret_cast<const char*>(ptrs));
            }
        }
        if (needed <= DIRECT_COUNT && n.indirect) { freeBlock(n.indirect); n.indirect = 0; }
        n.size = static_cast<uint32_t>(data.size()); return true;
    }
    bool dirEntries(uint32_t id, vector<DirEntry>& entries) {
        Inode n{}; if (!loadInode(id, n) || n.type != TYPE_DIR) return false;
        string raw; if (!readBytes(n, raw)) return false;
        entries.clear();
        for (size_t p = 0; p + sizeof(DirEntry) <= raw.size(); p += sizeof(DirEntry)) {
            DirEntry e{}; memcpy(&e, raw.data() + p, sizeof(e)); if (e.inode != UINT32_MAX) entries.push_back(e);
        }
        return true;
    }
    bool saveDirEntries(uint32_t id, const vector<DirEntry>& entries) {
        Inode n{}; if (!loadInode(id, n)) return false;
        string raw(entries.size() * sizeof(DirEntry), '\0');
        for (size_t i = 0; i < entries.size(); ++i) memcpy(raw.data() + i * sizeof(DirEntry), &entries[i], sizeof(DirEntry));
        if (!writeBytes(n, raw)) return false; return saveInode(id, n);
    }
    bool splitPath(const string& path, uint32_t& parent, string& name) {
        if (path.empty() || path[0] != '/' || path == "/") return false;
        vector<string> parts; string tmp; stringstream ss(path);
        while (getline(ss, tmp, '/')) if (!tmp.empty() && tmp != ".") { if (tmp == "..") { cout << "Переход .. в путях пока не поддерживается.\n"; return false; } parts.push_back(tmp); }
        if (parts.empty()) return false;
        name = parts.back(); if (name.size() >= sizeof(DirEntry::name)) return false; parts.pop_back();
        parent = sb.rootInode;
        for (const string& part : parts) {
            vector<DirEntry> es; if (!dirEntries(parent, es)) return false;
            bool found = false;
            for (const auto& e : es) if (part == e.name && e.type == TYPE_DIR) { parent = e.inode; found = true; break; }
            if (!found) return false;
        }
        return true;
    }
    bool findPath(const string& path, uint32_t& id) {
        if (path == "/") { id = sb.rootInode; return true; }
        uint32_t parent; string name; if (!splitPath(path, parent, name)) return false;
        vector<DirEntry> es; if (!dirEntries(parent, es)) return false;
        for (const auto& e : es) if (name == e.name) { id = e.inode; return true; }
        return false;
    }
    bool addEntry(uint32_t parent, const string& name, uint32_t id, uint8_t type) {
        vector<DirEntry> es; if (!dirEntries(parent, es)) return false;
        for (const auto& e : es) if (name == e.name) { cout << "Ошибка: имя уже существует.\n"; return false; }
        DirEntry e{}; e.inode = id; e.type = type; strncpy(e.name, name.c_str(), sizeof(e.name)-1); es.push_back(e);
        return saveDirEntries(parent, es);
    }
    bool removeEntry(uint32_t parent, const string& name, uint32_t& id, uint8_t& type) {
        vector<DirEntry> es; if (!dirEntries(parent, es)) return false;
        auto it = find_if(es.begin(), es.end(), [&](const DirEntry& e){ return name == e.name; });
        if (it == es.end()) return false; id = it->inode; type = it->type; es.erase(it); return saveDirEntries(parent, es);
    }
    uint32_t checksum(const char* data, size_t len) {
        uint32_t h = 2166136261u; for (size_t i=0;i<len;++i) { h ^= static_cast<unsigned char>(data[i]); h *= 16777619u; } return h;
    }
    bool beginTransaction() {
        // Write-ahead metadata snapshot: blocks 0..10 are copied before ACTIVE is set.
        char block[BLOCK_SIZE];
        for (uint32_t i=0;i<11;++i) { if (!readBlock(i, block) || !writeBlock(JOURNAL_BACKUP_START+i, block)) return false; }
        JournalHeader h{}; h.magic = 0x4A4E4C31; h.active = 1;
        char backup[11 * BLOCK_SIZE];
        for (uint32_t i=0;i<11;++i) readBlock(JOURNAL_BACKUP_START+i, backup+i*BLOCK_SIZE);
        h.checksum = checksum(backup, sizeof(backup));
        char hdr[BLOCK_SIZE]{}; memcpy(hdr, &h, sizeof(h)); return writeBlock(JOURNAL_HEADER_BLOCK, hdr);
    }
    void commitTransaction() {
        JournalHeader h{}; h.magic = 0x4A4E4C31; h.active = 0;
        char hdr[BLOCK_SIZE]{}; memcpy(hdr, &h, sizeof(h)); writeBlock(JOURNAL_HEADER_BLOCK, hdr);
    }
    bool recoverJournal() {
        char hdr[BLOCK_SIZE]{}; if (!readBlock(JOURNAL_HEADER_BLOCK, hdr)) return false;
        JournalHeader h{}; memcpy(&h, hdr, sizeof(h));
        if (h.magic != 0x4A4E4C31 || !h.active) return true;
        char backup[11 * BLOCK_SIZE];
        for (uint32_t i=0;i<11;++i) if (!readBlock(JOURNAL_BACKUP_START+i, backup+i*BLOCK_SIZE)) return false;
        if (checksum(backup, sizeof(backup)) != h.checksum) { cout << "Журнал повреждён: контрольная сумма не совпадает.\n"; return false; }
        for (uint32_t i=0;i<11;++i) if (!writeBlock(i, backup+i*BLOCK_SIZE)) return false;
        h.active = 0; memset(hdr, 0, BLOCK_SIZE); memcpy(hdr, &h, sizeof(h)); writeBlock(JOURNAL_HEADER_BLOCK, hdr);
        cout << "Восстановление: незавершённая транзакция отменена.\n"; return true;
    }
    bool requireMounted() const { if (!mounted) cout << "Сначала выполните mount или format.\n"; return mounted; }
public:
    ~FileSystem() { if (disk.is_open()) disk.close(); }
    bool format() {
        if (disk.is_open()) disk.close();
        ofstream out(IMAGE, ios::binary | ios::trunc); if (!out) { cout << "Не удалось создать образ.\n"; return false; }
        vector<char> zeros(static_cast<size_t>(TOTAL_BLOCKS)*BLOCK_SIZE, 0); out.write(zeros.data(), zeros.size()); out.close();
        disk.open(IMAGE, ios::in|ios::out|ios::binary); if (!disk) return false;
        sb = {MAGIC,1,BLOCK_SIZE,TOTAL_BLOCKS,INODE_COUNT,1,2,3,JOURNAL_HEADER_BLOCK,11,DATA_START,0};
        char b[BLOCK_SIZE]{}; memcpy(b,&sb,sizeof(sb)); writeBlock(0,b);
        memset(b,0,BLOCK_SIZE); for (uint32_t i=0;i<DATA_START;++i) b[i/8] |= static_cast<char>(1u<<(i%8)); writeBlock(1,b);
        memset(b,0,BLOCK_SIZE); b[0] = 1; writeBlock(2,b);
        Inode root{}; root.used=1; root.type=TYPE_DIR; root.links=1; root.created=nowSeconds();
        uint32_t rootBlock=allocBlock(); if (!rootBlock) return false; root.direct[0]=rootBlock; saveInode(0,root); bitSet(2,0,true);
        mounted=true; commitTransaction();
        cout << "Файловая система создана. Образ: " << IMAGE << " (" << (TOTAL_BLOCKS*BLOCK_SIZE/1024) << " КБ).\n";
        cout << "Блок данных начинается с " << DATA_START << "; корневой каталог inode 0.\n"; return true;
    }
    bool mount() {
        if (disk.is_open()) disk.close();
        disk.open(IMAGE, ios::in|ios::out|ios::binary); if (!disk) { cout << "Образ не найден. Выполните format.\n"; return false; }
        char b[BLOCK_SIZE]{}; if (!readBlock(0,b)) return false; memcpy(&sb,b,sizeof(sb));
        if (sb.magic!=MAGIC || sb.blockSize!=BLOCK_SIZE || sb.totalBlocks!=TOTAL_BLOCKS || sb.dataStart!=DATA_START) { cout << "Неверный формат образа.\n"; return false; }
        if (!recoverJournal()) return false;
        // Reload superblock after potential recovery.
        readBlock(0,b); memcpy(&sb,b,sizeof(sb)); mounted=true;
        cout << "Образ успешно смонтирован.\n"; return true;
    }
    bool create(const string& path, bool directory=false) {
        if (!requireMounted()) return false;
        uint32_t parent; string name; if (!splitPath(path,parent,name)) { cout << "Неверный путь или родительский каталог.\n"; return false; }
        if (!beginTransaction()) return false;
        uint32_t id=allocInode(directory?TYPE_DIR:TYPE_FILE);
        if (id==UINT32_MAX) { commitTransaction(); cout << "Нет свободных inode.\n"; return false; }
        if (!addEntry(parent,name,id,directory?TYPE_DIR:TYPE_FILE)) { freeInode(id); commitTransaction(); return false; }
        commitTransaction(); cout << (directory?"Каталог":"Файл") << " создан: " << path << " (inode " << id << ").\n"; return true;
    }
    bool writeFile(const string& path, const string& content) {
        if (!requireMounted()) return false; uint32_t id; if (!findPath(path,id)) { cout << "Файл не найден.\n"; return false; }
        Inode n{}; if (!loadInode(id,n) || n.type!=TYPE_FILE) { cout << "Это не файл.\n"; return false; }
        if (!beginTransaction()) return false; bool ok=writeBytes(n,content) && saveInode(id,n); commitTransaction();
        if (ok) cout << "Записано байт: " << content.size() << ".\n"; else cout << "Ошибка записи.\n"; return ok;
    }
    bool readFile(const string& path) {
        if (!requireMounted()) return false; uint32_t id; if (!findPath(path,id)) { cout << "Файл не найден.\n"; return false; }
        Inode n{}; string data; if (!loadInode(id,n) || n.type!=TYPE_FILE || !readBytes(n,data)) { cout << "Ошибка чтения.\n"; return false; }
        cout << "--- Содержимое (" << data.size() << " байт) ---\n" << data << "\n-------------------------------\n"; return true;
    }
    bool openFile(const string& path) { uint32_t id; if (!requireMounted() || !findPath(path,id)) { cout << "Файл не найден.\n"; return false; } Inode n{}; loadInode(id,n); cout << "Открыт: "<<path<<", inode="<<id<<", размер="<<n.size<<" байт.\n"; return true; }
    bool list(const string& path) {
        if (!requireMounted()) return false; uint32_t id; if (!findPath(path,id)) { cout << "Каталог не найден.\n"; return false; }
        Inode n{}; loadInode(id,n); if (n.type!=TYPE_DIR) { cout << "Указанный путь не каталог.\n"; return false; }
        vector<DirEntry> es; dirEntries(id,es); cout << "Содержимое "<<path<<":\n";
        for (auto&e:es) { Inode child{}; loadInode(e.inode,child); cout << (e.type==TYPE_DIR?"[DIR] ":"[FILE] ") << e.name << " inode="<<e.inode<<" size="<<child.size<<"\n"; } return true;
    }
    bool stat(const string& path) {
        if (!requireMounted()) return false; uint32_t id; if (!findPath(path,id)) { cout << "Объект не найден.\n"; return false; }
        Inode n{}; loadInode(id,n); uint32_t count=(n.size+BLOCK_SIZE-1)/BLOCK_SIZE;
        cout << "Путь: "<<path<<"\nInode: "<<id<<"\nТип: "<<(n.type==TYPE_DIR?"каталог":"файл")<<"\nРазмер: "<<n.size<<" байт\nЖёстких ссылок: "<<n.links<<"\nБлоков данных: "<<count<<"\n"; return true;
    }
    bool link(const string& oldPath,const string& newPath) {
        if (!requireMounted()) return false; uint32_t id,parent; string name;
        if (!findPath(oldPath,id) || !splitPath(newPath,parent,name)) { cout << "Неверный исходный или новый путь.\n"; return false; }
        Inode n{}; loadInode(id,n); if (n.type!=TYPE_FILE) { cout << "Жёсткие ссылки на каталоги запрещены.\n"; return false; }
        if (!beginTransaction()) return false; bool ok=addEntry(parent,name,id,TYPE_FILE); if (ok) { ++n.links; saveInode(id,n); } commitTransaction();
        if (ok) cout << "Жёсткая ссылка создана.\n"; return ok;
    }
    bool remove(const string& path) {
        if (!requireMounted()) return false; uint32_t parent,id; string name;
        if (!splitPath(path,parent,name)) { cout << "Неверный путь.\n"; return false; }
        vector<DirEntry> es; if (!dirEntries(parent,es)) return false;
        auto it=find_if(es.begin(),es.end(),[&](const DirEntry&e){return name==e.name;});
        if (it==es.end()) { cout << "Объект не найден.\n"; return false; }
        id=it->inode; uint8_t type=it->type; Inode n{}; loadInode(id,n);
        if (type==TYPE_DIR) { vector<DirEntry> child; dirEntries(id,child); if (!child.empty()) { cout << "Каталог не пуст.\n"; return false; } }
        if (!beginTransaction()) return false;
        uint32_t removed; uint8_t removedType; bool ok=removeEntry(parent,name,removed,removedType);
        if (ok) { if (n.links>1) { --n.links; saveInode(id,n); } else freeInode(id); }
        commitTransaction(); if (ok) cout << "Удалено: "<<path<<"\n"; return ok;
    }
    void info() {
        if (!requireMounted()) return; uint32_t freeB=0,freeI=0;
        for(uint32_t b=sb.dataStart;b<TOTAL_BLOCKS;++b) if(!bitGet(1,b)) ++freeB;
        for(uint32_t i=0;i<INODE_COUNT;++i) if(!bitGet(2,i)) ++freeI;
        cout << "Размер образа: "<<TOTAL_BLOCKS*BLOCK_SIZE<<" байт\nРазмер блока: "<<BLOCK_SIZE<<" байт\nВсего блоков: "<<TOTAL_BLOCKS<<"\nСвободных блоков: "<<freeB<<"\nВсего inode: "<<INODE_COUNT<<"\nСвободных inode: "<<freeI<<"\nМетаданные: 23 блока ("<<23*BLOCK_SIZE<<" байт, "<<fixed<<setprecision(2)<<(23.0/TOTAL_BLOCKS*100)<<"% образа)\n";
        cout << "Максимум файла: "<<(DIRECT_COUNT+BLOCK_SIZE/4)*BLOCK_SIZE<<" байт ("<<DIRECT_COUNT<<" прямых + "<<(BLOCK_SIZE/4)<<" косвенных указателей).\n";
    }
    void compare() {
        cout << "\n=== СРАВНЕНИЕ РАЗМЕЩЕНИЯ ФАЙЛОВ ===\n"
             << "Непрерывное: блоки идут подряд; быстрый последовательный и прямой доступ; внешняя фрагментация, сложно расширять файл.\n"
             << "Связное: каждый блок содержит указатель на следующий; внешней фрагментации размещения нет; произвольный доступ медленный, есть накладные расходы на указатели.\n"
             << "Индексное: адреса блоков хранятся в индексной структуре; удобен прямой доступ и расширение; расходуется место на индексные указатели.\n"
             << "В этой модели используется индексное размещение: 8 прямых указателей и один блок косвенных указателей.\n";
    }
    void fragmentation() {
        if (!requireMounted()) return; uint64_t usedBytes=0, allocated=0, fileCount=0;
        for(uint32_t i=0;i<INODE_COUNT;++i) if(bitGet(2,i)) { Inode n{}; loadInode(i,n); if(n.used && n.type==TYPE_FILE) { ++fileCount; usedBytes+=n.size; allocated+=((n.size+BLOCK_SIZE-1)/BLOCK_SIZE)*BLOCK_SIZE; } }
        cout << "\n=== ОЦЕНКА ФРАГМЕНТАЦИИ ===\nФайлов: "<<fileCount<<"\nПолезные данные: "<<usedBytes<<" байт\nВыделено под данные: "<<allocated<<" байт\nВнутренняя фрагментация: "<<(allocated-usedBytes)<<" байт (неиспользованный остаток последнего блока каждого файла).\n";
        cout << "Внешняя фрагментация: для индексной схемы нет требования непрерывного свободного участка; оценка крупнейшего непрерывного свободного участка: ";
        uint32_t best=0,run=0; for(uint32_t b=sb.dataStart;b<TOTAL_BLOCKS;++b) { if(!bitGet(1,b)) { ++run; best=max(best,run); } else run=0; }
        cout << best << " блок(а). Для сравнения непрерывной схемы этот показатель определяет размер крупнейшего файла, который можно разместить одним участком.\n";
        cout << "Примечание: оценка выполнена по текущему состоянию образа; размер блока фиксирован 1024 байта.\n";
    }
    void check() {
        if (!requireMounted()) return; uint32_t bad=0, entriesBad=0;
        for(uint32_t i=0;i<INODE_COUNT;++i) if(bitGet(2,i)) { Inode n{}; if(!loadInode(i,n)||!n.used||n.type<TYPE_FILE||n.type>TYPE_DIR) { ++bad; continue; }
            if(n.type==TYPE_DIR) { vector<DirEntry> es; if(!dirEntries(i,es)) ++entriesBad; else for(auto&e:es) if(e.inode>=INODE_COUNT||!bitGet(2,e.inode)) ++entriesBad; }
            uint32_t blocks=(n.size+BLOCK_SIZE-1)/BLOCK_SIZE; for(uint32_t k=0;k<blocks;++k) { uint32_t b; if(!getDataBlock(n,k,b)||b<sb.dataStart||b>=TOTAL_BLOCKS||!bitGet(1,b)) ++bad; }
        }
        cout << "Проверка образа завершена. Некорректных inode: "<<bad<<"; ошибок каталогов: "<<entriesBad<<".\n";
    }
    bool simulateCrash() {
        if (!requireMounted()) return false;
        if (!beginTransaction()) return false;
        // Deliberately leave the transaction ACTIVE. Next mount restores metadata snapshot.
        cout << "Имитация сбоя: журнал оставлен в состоянии ACTIVE. Выполните exit, затем снова запустите программу и mount.\n";
        return true;
    }
};

static void printHelp() {
    cout << "Команды:\n"
         << " format                         - создать новый образ (СТИРАЕТ старые данные)\n"
         << " mount                          - смонтировать filesystem.img и выполнить recovery\n"
         << " mkdir /path                    - создать каталог\n"
         << " create /path                   - создать пустой файл\n"
         << " open /path                     - открыть/проверить файл\n"
         << " write /path TEXT               - записать текст (заменяет содержимое)\n"
         << " read /path                     - прочитать файл\n"
         << " delete /path                   - удалить файл или пустой каталог\n"
         << " link /old /new                 - создать жёсткую ссылку на файл\n"
         << " ls [/]                         - список каталога\n"
         << " stat /path                     - сведения об объекте\n"
         << " info                           - параметры образа и накладные расходы\n"
         << " compare                        - сравнить способы размещения\n"
         << " fragmentation                  - оценить фрагментацию\n"
         << " fsck                           - проверить базовую целостность\n"
         << " crash                          - оставить незавершённую транзакцию для теста recovery\n"
         << " help                           - справка\n exit                           - выход\n";
}

int main() {
    FileSystem fs;
    cout << "=== FILE SYSTEM SIMULATOR ===\nВведите help для списка команд. Образ: " << IMAGE << "\n";
    string line;
    while (true) {
        cout << "fs> " << flush;
        if (!getline(cin,line)) break;
        if (line.empty()) continue;
        istringstream in(line); string cmd; in >> cmd;
        if (cmd=="exit" || cmd=="quit") break;
        if (cmd=="help") { printHelp(); continue; }
        if (cmd=="format") { fs.format(); continue; }
        if (cmd=="mount") { fs.mount(); continue; }
        if (cmd=="compare") { fs.compare(); continue; }
        if (cmd=="fragmentation") { fs.fragmentation(); continue; }
        if (cmd=="info") { fs.info(); continue; }
        if (cmd=="fsck") { fs.check(); continue; }
        if (cmd=="crash") { fs.simulateCrash(); continue; }
        if (cmd=="ls") { string p; in>>p; if(p.empty()) p="/"; fs.list(p); continue; }
        if (cmd=="create" || cmd=="mkdir" || cmd=="open" || cmd=="read" || cmd=="delete" || cmd=="stat") {
            string p; in>>p; if(p.empty()){ cout<<"Укажите путь.\n"; continue; }
            if(cmd=="create") fs.create(p,false); else if(cmd=="mkdir") fs.create(p,true); else if(cmd=="open") fs.openFile(p); else if(cmd=="read") fs.readFile(p); else if(cmd=="delete") fs.remove(p); else fs.stat(p); continue;
        }
        if (cmd=="write") { string p; in>>p; string content; getline(in,content); if(!content.empty()&&content[0]==' ') content.erase(0,1); fs.writeFile(p,content); continue; }
        if (cmd=="link") { string a,b; in>>a>>b; if(a.empty()||b.empty()) cout<<"Использование: link /старый /новый\n"; else fs.link(a,b); continue; }
        cout << "Неизвестная команда. Введите help.\n";
    }
    return 0;
}

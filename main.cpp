
#include <iostream>
#include <fstream>
#include <cstdint>
#include <cstring>

using namespace std;

const int BLOCK_SIZE = 1024;
const int TOTAL_BLOCKS = 1024;
const int INODE_COUNT = 64;

struct Superblock {
    uint32_t magic;
    uint32_t version;
    uint32_t blockSize;
    uint32_t totalBlocks;
    uint32_t inodeCount;
    uint32_t blockBitmapStart;
    uint32_t inodeBitmapStart;
    uint32_t inodeTableStart;
    uint32_t dataStart;
};

void createDisk() {
    // Создаём файл-образ
    ofstream disk("filesystem.img", ios::binary);

    if (!disk) {
        cout << "Ошибка создания диска!\n";
        return;
    }

    char emptyBlock[BLOCK_SIZE] = {};

    for (int i = 0; i < TOTAL_BLOCKS; i++) {
        disk.write(emptyBlock, BLOCK_SIZE);
    }

    disk.close();

    // Открываем образ для записи метаданных
    fstream fs("filesystem.img", ios::in | ios::out | ios::binary);

    if (!fs) {
        cout << "Ошибка открытия диска!\n";
        return;
    }

    // Создаём суперблок
    Superblock sb = {};

    sb.magic = 0x12345678;
    sb.version = 1;
    sb.blockSize = BLOCK_SIZE;
    sb.totalBlocks = TOTAL_BLOCKS;
    sb.inodeCount = INODE_COUNT;

    sb.blockBitmapStart = 1;
    sb.inodeBitmapStart = 2;
    sb.inodeTableStart = 3;
    sb.dataStart = 7;

    // Записываем суперблок
    fs.write(reinterpret_cast<const char*>(&sb), sizeof(sb));

    // Создаём битовую карту блоков
    unsigned char blockBitmap[BLOCK_SIZE] = {};

    // Первые 7 блоков заняты метаданными
    for (int i = 0; i < 7; i++) {
        blockBitmap[i / 8] |= (1 << (i % 8));
    }

    fs.seekp(BLOCK_SIZE);
    fs.write(reinterpret_cast<char*>(blockBitmap), BLOCK_SIZE);

    // Создаём битовую карту inode
    unsigned char inodeBitmap[BLOCK_SIZE] = {};

    fs.seekp(BLOCK_SIZE * 2);
    fs.write(reinterpret_cast<char*>(inodeBitmap), BLOCK_SIZE);

    fs.close();

    cout << "\nФайловая система создана!\n";
    cout << "Размер диска: 1024 КБ\n";
    cout << "Размер блока: 1024 байта\n";
    cout << "Количество блоков: 1024\n";
    cout << "Количество inode: 64\n";
    cout << "Начало области данных: блок 7\n";
    cout << "Суперблок записан успешно!\n";
}

int main() {
    int choice;

    cout << "=============================\n";
    cout << "   FILE SYSTEM SIMULATOR\n";
    cout << "=============================\n";

    cout << "1. Создать файловую систему\n";
    cout << "2. Выход\n";
    cout << "Выбор: ";

    cin >> choice;

    if (choice == 1) {
        createDisk();
    }
    else {
        cout << "Выход из программы.\n";
    }

    return 0;
}

#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/stat.h>
#ifdef _WIN32
    #include <direct.h>
    #define MKDIR(p) _mkdir(p)
#else
    #define MKDIR(p) mkdir((p), 0755)
#endif

#define MAX_SELECTED_BLOCKS 10
#define MAX_PATH 260
#define MAX_FILE_SIZE 0xffff - 2
#define APP_VERSION "1.07"

#define DT_BASIC     0
#define DT_NUMARRAY  1
#define DT_CHARARRAY 2
#define DT_CODE      3

#define STR_HELPER(x) #x
#define STR(x) STR_HELPER(x)
#define MAX_SELECTED_BLOCKS_S STR(MAX_SELECTED_BLOCKS)
#ifdef BUILD_TS
    #define BUILD_TS_S STR(BUILD_TS)
#else
    #define BUILD_TS_S "none"
#endif

typedef unsigned char byte;

//~~~~ Global variables ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~//

enum command
{
    cm_Unknown,
    cm_Add,
    cm_Insert,
    cm_Extract,
    cm_List,
    cm_Remove,
    cm_Replace,
    cm_Fix0,
    cm_FixCrc,
    cm_ExportPng,
    cm_ExportAll,
    cm_Info
} Command
    = cm_Unknown;

struct tapeheader
{
    byte LenLo1;
    byte LenHi1;
    byte Flag1;
    byte HType;
    char HName[10];
    byte HLenLo;
    byte HLenHi;
    byte HStartLo;
    byte HStartHi;
    byte HParam2Lo;
    byte HParam2Hi;
    byte Parity1;
} TapeHeader = {
        19, 0,                                        // len
        0,                                            // flag
        DT_CODE,                                      // datatype
        { 32, 32, 32, 32, 32, 32, 32, 32, 32, 32 },   // name
        0, 0,                                         // length of data block
        0x00, 0x00,                                   // param1 : origin
        0x00, 0x80,                                   // param2
        0,                                            // checksum
    };

struct selectedblocks
{
    int count;
    struct
    {
        int start;
        int end;
    } block[MAX_SELECTED_BLOCKS];
} SelectedBlocks;

bool AddTapeHeader = false;
bool RawData = false;
bool QuietMode = false;
bool WriteInf = false;          // export-all: also emit .inf sidecars
int  BasicVarsParam = -1;       // BASIC param2 (prog length before vars); -1 = auto
const char *InfPath = NULL;     // --from-inf <file> source for header reconstruction

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~//

void showUsage(void)
{
    puts(
    "TAPe UTility v" APP_VERSION " by Sivvus (build:" BUILD_TS_S ")\n"
    "Usage: TAPUT command [options] FileIn [FileOut]\n"
    "Commands:\n"
    "    add              Add a file at the end of the \"tap\" image\n"
    "                     if the image does not exist, it will be created\n"
    "    export-all       Extract every data block to its own file in the\n"
    "                     given folder (named after the header); --inf also\n"
    "                     writes a .inf metadata sidecar per file\n"
    "    export-png       Render a SCREEN$ block (or .scr file with no -s)\n"
    "                     to a 256x192 PNG image\n"
    "    extract          Extract a block of data to a file\n"
    "    fix-0            Remove empty blocks from the image\n"
    "    fix-crc          Correct the checksum of the selected blocks\n"
    "    info             Show a summary of the image (block counts, integrity)\n"
    "    insert           Insert a file at the beginning of the image,\n"
    "                     with the -s <n> option inserts a file before block n\n"
    "    list             List image content\n"
    "    remove           Remove blocks of data from the image (requires -s (<n>|<n>-<m>))\n"
    "                     up to " MAX_SELECTED_BLOCKS_S " blocks can be selected\n"
    "    replace          Replace the data block with the contents of the file\n"
    "Options:\n"
    "    -b               Creates a BASIC program header block\n"
    "    --from-inf <f>   Read header name/type/address from a .inf file\n"
    "                     (use with add / insert / replace)\n"
    "    -h, --help       Display this help and exit\n"
    "    --inf            With export-all, also write .inf metadata sidecars\n"
    "    -n <name>        Implies creating a block header,\n"
    "                     <name> sets a block name\n"
    "    -o <addr>        Implies creating a block header,\n"
    "                     <addr> sets the origin address or the BASIC autostart line number\n"
    "    -r, --raw        Treats input/output files as raw data,\n"
    "                     refrains from creating a flag and checksum\n"
    "    -s (<n>|<n>-<m>)[,(<n>|<n>-<m>)]..\n"
    "                     Selects the block numbers and/or block ranges (up to " MAX_SELECTED_BLOCKS_S ")\n"
    "Examples:\n"
    "    taput add -o 32768 -n \"Block name\" file.bin image.tap\n"
    "    taput export-all --inf image.tap ./out\n"
    "    taput export-png -s 2 image.tap screen.png\n"
    "    taput extract -s 2 image.tap file.bin\n"
    "    taput info image.tap\n"
    "    taput fix-0 image.tap\n"
    "    taput fix-crc -s 3 image.tap\n"
    "    taput insert -s 3 -o 32768 -n \"Block name\" file.bin image.tap\n"
    "    taput list image.tap\n"
    "    taput remove -s 1-4,8,12 image.tap\n"
    "    taput replace -s 2 file.bin image.tap"
    );
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~//

byte crc(byte *start, int length)
{
    byte tcrc = 0;
    for (int i = 0; i < length; i++)
        tcrc = tcrc ^ start[i];
    return tcrc;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~//

void PrepareTapeHeader(int size)
{
    switch (TapeHeader.HType)
    {
        case DT_BASIC:
        {
            // param2 = length of the program before its variables. When a
            // .inf supplied an explicit value (BasicVarsParam) keep it,
            // otherwise default to the whole size (no variables saved).
            int p2 = (BasicVarsParam >= 0) ? BasicVarsParam : size;
            TapeHeader.HParam2Lo = (byte)(p2 & 0xff);
            TapeHeader.HParam2Hi = (byte)(p2 >> 8);
            break;
        }
        case DT_CODE:
            TapeHeader.HParam2Lo = 0x00;
            TapeHeader.HParam2Hi = 0x80;
            break;
    }
    if (RawData)
        size -= 2;
    if (size < 0) size = 0;
    TapeHeader.HLenLo = (byte)(size & 0xff);
    TapeHeader.HLenHi = (byte)(size >> 8);
    TapeHeader.Parity1 = crc(&TapeHeader.Flag1, 18);
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~//

void chkOutFile(char *FileName)
{
    if (FileName[0] == '\0')
    {
        fprintf(stderr, "No output file name was given\n");
        exit(1);
    }
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~//

void writeBlock(void *start, size_t size, FILE *file, const char *fileName)
{
    if (!size) return;
    if (!fwrite(start, size, 1, file))
    {
        fprintf(stderr, "Unable to write file \"%s\"\n", fileName);
        fclose(file);
        exit(1);
    }    
}

//~~~~ LoadFile ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~//
// load file to memory 
// return pionter and size - size of file
// if fail return NULL

byte *LoadFile(size_t *size, const char *FileName)
{
    FILE *file;
    file = fopen(FileName, "rb");
    if (!file)
    {
        *size = 0;
        return NULL;
    }
    fseek(file, 0, SEEK_END);
    *size = ftell(file);
    fseek(file, 0, SEEK_SET);
    byte *Dest = (byte*)malloc(*size + 1);
    if (!Dest)
    {
        fprintf(stderr, "Memory allocation error\n");
        fclose(file);
        exit(1);
    }
    if (*size)
        if (!fread(Dest, *size, 1, file))
        {
            *size = 0;
            free(Dest);
            Dest = NULL;
        }
    fclose(file);

    return Dest;
}

//~~~~ isSelected ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~//
// check if given block numbrer is selected

bool isSelected(int BlockNo)
{
    for (int i = 0; i < SelectedBlocks.count; i++)
        if (
                SelectedBlocks.block[i].start <= SelectedBlocks.block[i].end &&
                BlockNo >= SelectedBlocks.block[i].start &&
                BlockNo <= SelectedBlocks.block[i].end
            )
            return true;
    return false;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~//

void cmdAdd(const char *FileNameIn, const char *FileNameOut)
{
    size_t sizeIn;
    byte *bufferIn = LoadFile(&sizeIn, FileNameIn);
    if (!bufferIn)
    {
        fprintf(stderr, "Unable to open file \"%s\"\n", FileNameIn);
        exit(1);
    }
    if (sizeIn > MAX_FILE_SIZE)
    {
        fprintf(stderr, "The file \"%s\" is too large\n", FileNameIn);
        free(bufferIn);
        exit(1);
    }

    size_t sizeOut;
    byte *bufferOut = LoadFile(&sizeOut, FileNameOut);
    byte *endbuf = bufferOut + sizeOut;
    byte *pos = bufferOut;

    FILE *file;
    file = fopen(FileNameOut, "wb");
    if (!file)
    {
        fprintf(stderr, "Unable to create file \"%s\"\n", FileNameOut);
        free(bufferOut);
        free(bufferIn);
        exit(1);
    }

    // re-write old file
    if (bufferOut)
    {
        while (pos < endbuf)
        {
            size_t blocksize = pos[0] | (pos[1] << 8);

            if ((pos + blocksize + 1) < endbuf)
                writeBlock(pos, blocksize + 2, file, FileNameOut);
            else
           {
                writeBlock(pos, endbuf - pos, file, FileNameOut);
                fclose(file);
                free(bufferOut);
                free(bufferIn);
                fprintf(stderr, "Warning: image \"%s\" is corrupted", FileNameOut);
                fprintf(stderr, ", saving data cannot be done\n");
                exit(1);
            }

            pos += blocksize + 2;
        }
        free(bufferOut);
    }

    // add new part
    if (AddTapeHeader)
    {
        PrepareTapeHeader(sizeIn);
        writeBlock(&TapeHeader, sizeof(TapeHeader), file, FileNameOut);
    }
    byte Head[3];
    if (RawData)
    {
        Head[0] = sizeIn & 0xff;
        Head[1] = sizeIn >> 8;
        writeBlock(Head, 2, file, FileNameOut);
        writeBlock(bufferIn, sizeIn, file, FileNameOut);
    }
    else
    {
        Head[0] = (sizeIn + 2) & 0xff;
        Head[1] = (sizeIn + 2) >> 8;
        Head[2] = 0xff;
        writeBlock(Head, sizeof(Head), file, FileNameOut);
        writeBlock(bufferIn, sizeIn, file, FileNameOut);
        byte Parity = crc(bufferIn, sizeIn) ^ Head[2];
        writeBlock(&Parity, sizeof(Parity), file, FileNameOut);
    }
    free(bufferIn);
    fclose(file);
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~//

void cmdInsert(const char *FileNameIn, const char *FileNameOut)
{
    if (SelectedBlocks.count == 0)
    {
        SelectedBlocks.count = 1;
        SelectedBlocks.block[0].start = 1;
        SelectedBlocks.block[0].end = 1;
    }
    if (SelectedBlocks.count > 1 || SelectedBlocks.block[0].start != SelectedBlocks.block[0].end)
    {
        fprintf(stderr, "Exactly one block should be selected\n");
        exit(1);
    }
    bool isDone = false;
    
    // Load and check input file
    size_t sizeIn;
    byte *bufferIn = LoadFile(&sizeIn, FileNameIn);
    if (!bufferIn)
    {
        fprintf(stderr, "Unable to open file \"%s\"\n", FileNameIn);
        exit(1);
    }
    if (sizeIn > MAX_FILE_SIZE)
    {
        fprintf(stderr, "The file \"%s\" is too large\n", FileNameIn);
        free(bufferIn);
        exit(1);
    }

    // Load output file
    size_t sizeOut;
    byte *bufferOut = LoadFile(&sizeOut, FileNameOut);
    if (!bufferOut)
    {
        fprintf(stderr, "Unable to open file \"%s\"\n", FileNameOut);
        free(bufferIn);
        exit(1);
    }

    byte *endbuf = bufferOut + sizeOut;
    byte *pos = bufferOut;
    FILE *file;
    file = fopen(FileNameOut, "wb");
    if (!file)
    {
        fprintf(stderr, "Unable to create file \"%s\"\n", FileNameOut);
        free(bufferOut);
        free(bufferIn);
        exit(1);
    }
    for (int i = 1; pos < endbuf; i++)
    {
        if (isSelected(i))
        {
            if (AddTapeHeader)
            {
                PrepareTapeHeader(sizeIn);
                writeBlock(&TapeHeader, sizeof(TapeHeader), file, FileNameOut);
            }
            byte Head[3];
            if (RawData)
            {
                Head[0] = sizeIn & 0xff;
                Head[1] = sizeIn >> 8;
                writeBlock(Head, 2, file, FileNameOut);
                writeBlock(bufferIn, sizeIn, file, FileNameOut);
            } else {
                Head[0] = (sizeIn + 2) & 0xff;
                Head[1] = (sizeIn + 2) >> 8;
                Head[2] = 0xff;
                writeBlock(Head, sizeof(Head), file, FileNameOut);
                writeBlock(bufferIn, sizeIn, file, FileNameOut);
                byte Parity = crc(bufferIn, sizeIn) ^ Head[2];
                writeBlock(&Parity, sizeof(Parity), file, FileNameOut);
            }
            isDone = true;
        }
        size_t blocksize = pos[0] | (pos[1] << 8);
        if ((pos + blocksize + 1) < endbuf)
            writeBlock(pos, blocksize + 2, file, FileNameOut);
        else
            writeBlock(pos, endbuf - pos, file, FileNameOut);
        pos += blocksize + 2;
    }
    fclose(file);
    free(bufferOut);
    free(bufferIn);

    if (!isDone)
    {
        fprintf(stderr, "Selected block was not found\n");
        exit(1);
    }
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~//

void cmdReplace(const char *FileNameIn, const char *FileNameOut)
{
    if (SelectedBlocks.count != 1)
    {
        fprintf(stderr, "Exactly one block or range of blocks should be selected\n");
        exit(1);
    }

    bool isDone = false;

    size_t sizeIn;
    byte *bufferIn = LoadFile(&sizeIn, FileNameIn);
    if (!bufferIn)
    {
        fprintf(stderr, "Unable to open file \"%s\"\n", FileNameIn);
        exit(1);
    }
    if (sizeIn > MAX_FILE_SIZE)
    {
        fprintf(stderr, "The file \"%s\" is too large\n", FileNameIn);
        free(bufferIn);
        exit(1);
    }

    size_t sizeOut;
    byte *bufferOut = LoadFile(&sizeOut, FileNameOut);
    if (!bufferOut)
    {
        fprintf(stderr, "Unable to open file \"%s\"\n", FileNameOut);
        free(bufferIn);
        exit(1);
    }

    byte *endbuf = bufferOut + sizeOut;
    byte *pos = bufferOut;
    FILE *file;
    file = fopen(FileNameOut, "wb");
    if (!file)
    {
        fprintf(stderr, "Unable to create file \"%s\"\n", FileNameOut);
        free(bufferOut);
        free(bufferIn);
        exit(1);
    }
    for (int i = 1; pos < endbuf; i++)
    {
        size_t blocksize = pos[0] | (pos[1] << 8);
        if (isSelected(i))
        {
            if (!isDone)
            {
                if (AddTapeHeader)
                {
                    PrepareTapeHeader(sizeIn);
                    writeBlock(&TapeHeader, sizeof(TapeHeader), file, FileNameOut);
                }
                byte Head[3];
                if (RawData)
                {
                    Head[0] = sizeIn & 0xff;
                    Head[1] = sizeIn >> 8;
                    writeBlock(Head, 2, file, FileNameOut);
                    writeBlock(bufferIn, sizeIn, file, FileNameOut);
                } else {
                    Head[0] = (sizeIn + 2) & 0xff;
                    Head[1] = (sizeIn + 2) >> 8;
                    Head[2] = 0xff;
                    writeBlock(Head, sizeof(Head), file, FileNameOut);
                    writeBlock(bufferIn, sizeIn, file, FileNameOut);
                    byte Parity = crc(bufferIn, sizeIn) ^ Head[2];
                    writeBlock(&Parity, sizeof(Parity), file, FileNameOut);
                }
                isDone = true;
            }
        }
        else
        {
            if ((pos + blocksize + 1) < endbuf)
                writeBlock(pos, blocksize + 2, file, FileNameOut);
            else
                writeBlock(pos, endbuf - pos, file, FileNameOut);
        }
        pos += blocksize + 2;
    }

    fclose(file);
    free(bufferOut);
    free(bufferIn);

    if (!isDone)
    {
        fprintf(stderr, "Selected blocks were not found\n");
        exit(1);
    }
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~//

void cmdExtract(const char *FileNameIn, const char *FileNameOut)
{
    if (SelectedBlocks.count != 1 || SelectedBlocks.block[0].start != SelectedBlocks.block[0].end)
    {
        fprintf(stderr, "Exactly one block should be selected\n");
        exit(1);
    }

    bool isDone = false;
    bool isComplete = true;

    size_t size;
    byte *buffer = LoadFile(&size, FileNameIn);
    if (!buffer)
    {
        fprintf(stderr, "Unable to open file \"%s\"\n", FileNameIn);
        exit(1);
    }
    byte *endbuf = buffer + size;
    byte *pos = buffer;
    for (int i = 1; pos < endbuf; i++)
    {
        size_t blocksize = pos[0] | (pos[1] << 8);
        if (isSelected(i))
        {
            FILE *file;
            file = fopen(FileNameOut, "wb");
            if (!file)
            {
                fprintf(stderr, "Unable to create file \"%s\"\n", FileNameOut);
                free(buffer);
                exit(1);
            }
            if (RawData)
            {
                int len = endbuf - pos - 2;
                if (len >= (int)blocksize)
                    len = blocksize;
                else
                    isComplete = false;
                writeBlock(pos + 2, len, file, FileNameOut);
            } else {
                int len = endbuf - pos - 3;
                if (len >= (int)(blocksize - 2))
                    len = blocksize - 2;
                else
                    isComplete = false;
                writeBlock(pos + 3, len, file, FileNameOut);
            }
            fclose(file);
            isDone = true;
        }
        pos += blocksize + 2;
    }
    free(buffer);

    if (!isDone)
    {
        fprintf(stderr, "Selected block was not found\n");
        exit(1);
    }

    if (!isComplete)
    {
        fprintf(stderr, "Warning: image \"%s\" is corrupted", FileNameIn);
        fprintf(stderr, ", file \"%s\" has not been saved completely.\n",
            FileNameOut);
        exit(1);
    }
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~//

void cmdRemove(const char *FileName)
{
    if (SelectedBlocks.count == 0)
    {
        fprintf(stderr, "No block selected\n");
        exit(1);
    }

    bool isDone = false;

    size_t size;
    byte *buffer = LoadFile(&size, FileName);
    if (!buffer)
    {
        fprintf(stderr, "Unable to open file \"%s\"\n", FileName);
        exit(1);
    }
    byte *endbuf = buffer + size;
    byte *pos = buffer;
    FILE *file;
    file = fopen(FileName, "wb");
    if (!file)
    {
        fprintf(stderr, "Unable to create file \"%s\"\n", FileName);
        free(buffer);
        exit(1);
    }
    for (int i = 1; pos < endbuf; i++)
    {
        size_t blocksize = pos[0] | (pos[1] << 8);
        if (!isSelected(i))
        {
            if ((pos + blocksize + 1) < endbuf)
                writeBlock(pos, blocksize + 2, file, FileName);
            else
                writeBlock(pos, endbuf - pos, file, FileName);
        }
        else isDone = true;
        pos += blocksize + 2;
    }
    fclose(file);
    free(buffer);

    if (!isDone)
    {
        fprintf(stderr, "Selected blocks were not found\n");
        exit(1);
    }
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~//

void cmdFix0(const char *FileName)
{
    bool isDone = false;
    size_t size;
    byte *buffer = LoadFile(&size, FileName);
    if (!buffer)
    {
        fprintf(stderr, "Unable to open file \"%s\"\n", FileName);
        exit(1);
    }
    byte *endbuf = buffer + size;
    byte *pos = buffer;
    FILE *file;
    file = fopen(FileName, "wb");
    if (!file)
    {
        fprintf(stderr, "Unable to create file \"%s\"\n", FileName);
        free(buffer);
        exit(1);
    }
    while (pos < endbuf)
    {
        size_t blocksize = pos[0] | (pos[1] << 8);
        if (blocksize)
        {

            if ((pos + blocksize + 1) < endbuf)
                writeBlock(pos, blocksize + 2, file, FileName);
            else {
                writeBlock(pos, endbuf - pos, file, FileName);
                byte fill = 0;
                for (int cnt = (blocksize + 2)-(endbuf - pos); cnt > 0; cnt--)
                    writeBlock(&fill, 1, file, FileName);
                isDone = true;
            }
        }
        else
            isDone = true;
        pos += blocksize + 2;
    }
    fclose(file);
    free(buffer);

    if (!isDone)
    {
        fprintf(stderr, "No empty blocks found\n");
        exit(1);
    }
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~//

void cmdFixCrc(const char *FileName)
{
    if (SelectedBlocks.count != 1)
    {
        fprintf(stderr, "No block selected\n");
        exit(1);
    }

    bool isDone = false;
    bool isCorrupted = false;
    size_t size;
    byte *buffer = LoadFile(&size, FileName);
    if (!buffer)
    {
        fprintf(stderr, "Unable to open file \"%s\"\n", FileName);
        exit(1);
    }
    byte *endbuf = buffer + size;
    byte *pos = buffer;
    FILE *file;
    file = fopen(FileName, "wb");
    if (!file)
    {
        fprintf(stderr, "Unable to create file \"%s\"\n", FileName);
        free(buffer);
        exit(1);
    }
    for (int i = 1; pos < endbuf; i++)
    {
        size_t blocksize = pos[0] | (pos[1] << 8);
            if ((pos + blocksize + 1) < endbuf)
            {
                if (isSelected(i) && blocksize > 1)
                {
                    byte ccrc = crc(&pos[2], blocksize - 1);
                    if (pos[blocksize + 1] != ccrc)
                    {
                        pos[blocksize + 1] = ccrc;
                        isDone = true;
                    }
                }
                writeBlock(pos, blocksize + 2, file, FileName);
            } else {
                writeBlock(pos, endbuf - pos, file, FileName);
                if (isSelected(i)) isCorrupted = true;
            }
            
        pos += blocksize + 2;
    }
    fclose(file);
    free(buffer);
    
    if (isCorrupted)
    {
        fprintf(stderr, "Warning: image \"%s\" is corrupted\n", FileName);
    }
    
    if (!isDone)
    {
        fprintf(stderr, "No changes have been made\n");
        exit(1);
    }
    
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~//

void cmdList(const char *FileNameIn)
{
    size_t size;
    byte *buffer = LoadFile(&size, FileNameIn);
    if (!buffer)
    {
        fprintf(stderr, "Unable to open file \"%s\"\n", FileNameIn);
        exit(1);
    }
    byte *endbuf = buffer + size;
    byte *pos = buffer;
    for (int i = 1; pos < endbuf; i++)
    {
        size_t blocksize;
        if (pos + 1 < endbuf)
            blocksize = pos[0] | (pos[1] << 8);
        else
            blocksize = pos[0];
        int datasize = blocksize - 2;
        if (datasize < 0)
            datasize = 0;
        // check crc
        char crcStr[8];
        int crcCalc;
        if (blocksize > 1 && (pos + blocksize + 1) < endbuf)
        {
            crcCalc = crc(&pos[2], datasize + 1);
            if (pos[blocksize + 1] == crcCalc)
                sprintf(crcStr, "<%.2X>",crcCalc);
            else
                sprintf(crcStr, "<%.2X:%.2X>", (int)pos[blocksize + 1], crcCalc);
        } else
            strcpy(crcStr, "<-->");
        // blocktype
        char typeStr[7];
        if (blocksize && &pos[2] < endbuf)
        {
            switch (pos[2])
            {
                case 0x00:
                    if (blocksize == 19)
                        strcpy(typeStr, "<HEAD>");
                    else
                        sprintf(typeStr, "<0x%.2X>", (int)pos[2]);
                    break;
                case 0xff:
                    strcpy(typeStr, "<DATA>");
                    break;
                default:
                    sprintf(typeStr, "<0x%.2X>", (int)pos[2]);
                    break;
            };
        } else
            strcpy(typeStr, "<---->");

        printf("#%d\t%s\t%5d\t%s\t%.6X\t", i, typeStr, datasize, crcStr,
            (unsigned int)(pos - buffer));

        if (!blocksize)
            puts("empty block");
        else if (pos[2] == 0x00 && blocksize == 19)
        {
            struct tapeheader *Header = (struct tapeheader*)pos;
            char blockname[11];
            for (int j = 0; j < 10; j++)
                if (isprint(Header->HName[j]))
                    blockname[j] = Header->HName[j];
                else
                    blockname[j] = '?';
            blockname[10] = '\0';
            int startadr = Header->HStartLo | (Header->HStartHi << 8);
            int length = Header->HLenLo | (Header->HLenHi << 8);
            int basic = Header->HParam2Lo | (Header->HParam2Hi << 8);
            int variab = length - basic;
            switch (Header->HType)
            {
                case DT_BASIC:
                    printf("Program: %s\t", blockname);
                    if (startadr != 0x8000)
                        printf("LINE %d,", startadr);
                    printf("%d", length);
                    if (variab > 0)
                        printf("(B:%d,V:%d)\n", basic, variab);
                    else
                        printf("\n");
                    break;
                case DT_NUMARRAY:
                    printf("Number array: %s\n", blockname);
                    break;
                case DT_CHARARRAY:
                    printf("Character array: %s\n", blockname);
                    break;
                case DT_CODE:
                    printf("Bytes:   %s\tCODE %d,%d\n", blockname, startadr, length);
                    break;
                default:
                    printf("Unknown data type [0x%.2X]\n", Header->HType);
                    break;
            }
        }
        else
            puts("");
        pos += blocksize + 2;
    }
    if (pos != endbuf)
    {
        fprintf(stderr, "Warning: image \"%s\" is corrupted", FileNameIn);
        printf(", missing bytes: %d\n", (int)(pos - endbuf));
    }
}

//~~~~ PNG export ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~//
// Renders a ZX Spectrum SCREEN$ (6912 bytes: 6144 pixel + 768 attribute)
// to a 256x192 truecolour PNG. The encoder is self-contained: it emits
// stored (uncompressed) DEFLATE blocks so no zlib dependency is needed.

#define SCR_PIXELS 6144
#define SCR_SIZE   6912
#define SCR_W      256
#define SCR_H      192

// CRC-32 (IEEE) over a buffer, chained via the running (pre-inverted) value.
uint32_t crc32buf(const byte *buf, size_t len, uint32_t crc)
{
    for (size_t n = 0; n < len; n++)
    {
        crc ^= buf[n];
        for (int k = 0; k < 8; k++)
            crc = (crc & 1) ? (crc >> 1) ^ 0xedb88320UL : crc >> 1;
    }
    return crc;
}

uint32_t adler32buf(const byte *data, size_t len)
{
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < len; i++)
    {
        a = (a + data[i]) % 65521;
        b = (b + a) % 65521;
    }
    return (b << 16) | a;
}

// Wrap raw data in a zlib stream using only stored DEFLATE blocks.
byte *zlibStore(const byte *data, size_t len, size_t *outLen)
{
    size_t nblocks = (len + 65534) / 65535;
    if (nblocks == 0) nblocks = 1;
    byte *out = (byte*)malloc(2 + nblocks * 5 + len + 4);
    if (!out) return NULL;

    size_t p = 0;
    out[p++] = 0x78;            // CMF: deflate, 32K window
    out[p++] = 0x01;            // FLG: check bits, no preset dict
    size_t off = 0;
    do {
        size_t chunk = len - off;
        if (chunk > 65535) chunk = 65535;
        uint16_t n = (uint16_t)chunk;
        out[p++] = (off + chunk >= len) ? 1 : 0;   // BFINAL, BTYPE=00 (stored)
        out[p++] = n & 0xff;
        out[p++] = (n >> 8) & 0xff;
        out[p++] = (~n) & 0xff;                     // NLEN = ~LEN
        out[p++] = (~n >> 8) & 0xff;
        if (chunk) memcpy(out + p, data + off, chunk);
        p += chunk;
        off += chunk;
    } while (off < len);

    uint32_t ad = adler32buf(data, len);
    out[p++] = (ad >> 24) & 0xff;
    out[p++] = (ad >> 16) & 0xff;
    out[p++] = (ad >> 8) & 0xff;
    out[p++] = ad & 0xff;
    *outLen = p;
    return out;
}

void writePngChunk(FILE *file, const char *type, const byte *data,
                   size_t len, const char *fileName)
{
    byte lenb[4] = { (byte)(len >> 24), (byte)(len >> 16),
                     (byte)(len >> 8), (byte)len };
    writeBlock(lenb, 4, file, fileName);
    writeBlock((void*)type, 4, file, fileName);
    if (len) writeBlock((void*)data, len, file, fileName);

    uint32_t c = crc32buf((const byte*)type, 4, 0xffffffffUL);
    c = crc32buf(data, len, c) ^ 0xffffffffUL;
    byte crcb[4] = { (byte)(c >> 24), (byte)(c >> 16),
                     (byte)(c >> 8), (byte)c };
    writeBlock(crcb, 4, file, fileName);
}

void writePng(const byte *screen, const char *FileNameOut)
{
    size_t rawLen = (size_t)SCR_H * (1 + SCR_W * 3);
    byte *raw = (byte*)malloc(rawLen);
    if (!raw)
    {
        fprintf(stderr, "Memory allocation error\n");
        exit(1);
    }

    size_t rp = 0;
    for (int y = 0; y < SCR_H; y++)
    {
        raw[rp++] = 0;          // filter type 0 (none)
        for (int x = 0; x < SCR_W; x++)
        {
            int off = (x >> 3) + (y & 7) * 256
                    + ((y >> 3) & 7) * 32 + (y >> 6) * 2048;
            int on = (screen[off] >> (7 - (x & 7))) & 1;
            byte attr = screen[SCR_PIXELS + (y >> 3) * 32 + (x >> 3)];
            int colour = on ? (attr & 7) : ((attr >> 3) & 7);
            int v = (attr & 0x40) ? 255 : 205;     // bright bit
            raw[rp++] = (colour & 2) ? v : 0;      // red
            raw[rp++] = (colour & 4) ? v : 0;      // green
            raw[rp++] = (colour & 1) ? v : 0;      // blue
        }
    }

    size_t zlen;
    byte *zdata = zlibStore(raw, rawLen, &zlen);
    free(raw);
    if (!zdata)
    {
        fprintf(stderr, "Memory allocation error\n");
        exit(1);
    }

    FILE *file = fopen(FileNameOut, "wb");
    if (!file)
    {
        fprintf(stderr, "Unable to create file \"%s\"\n", FileNameOut);
        free(zdata);
        exit(1);
    }

    static const byte sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    byte ihdr[13] = {
        (SCR_W >> 24) & 0xff, (SCR_W >> 16) & 0xff,
        (SCR_W >> 8) & 0xff, SCR_W & 0xff,
        (SCR_H >> 24) & 0xff, (SCR_H >> 16) & 0xff,
        (SCR_H >> 8) & 0xff, SCR_H & 0xff,
        8,      // bit depth
        2,      // colour type: truecolour RGB
        0, 0, 0 // compression, filter, interlace
    };
    writeBlock((void*)sig, 8, file, FileNameOut);
    writePngChunk(file, "IHDR", ihdr, 13, FileNameOut);
    writePngChunk(file, "IDAT", zdata, zlen, FileNameOut);
    writePngChunk(file, "IEND", NULL, 0, FileNameOut);
    fclose(file);
    free(zdata);
}

// Copy SCREEN$ data into a full 6912-byte buffer. When attribute data is
// absent (only pixel data supplied), default to black ink on white paper.
void loadScreen(byte *screen, const byte *src, int len)
{
    if (len > SCR_SIZE) len = SCR_SIZE;
    if (len < 0) len = 0;
    memset(screen, 0, SCR_SIZE);
    if (len) memcpy(screen, src, len);
    if (len < SCR_SIZE)
        memset(screen + SCR_PIXELS, 0x38, 768);   // PAPER 7, INK 0
}

void cmdExportPng(const char *FileNameIn, const char *FileNameOut)
{
    byte screen[SCR_SIZE];

    // No block selected: treat the input as a standalone SCREEN$ (.scr) file.
    if (SelectedBlocks.count == 0)
    {
        size_t size;
        byte *buffer = LoadFile(&size, FileNameIn);
        if (!buffer)
        {
            fprintf(stderr, "Unable to open file \"%s\"\n", FileNameIn);
            exit(1);
        }
        if (size < SCR_PIXELS)
        {
            fprintf(stderr, "File \"%s\" is too small for a SCREEN$ "
                "(need at least %d bytes)\n", FileNameIn, SCR_PIXELS);
            free(buffer);
            exit(1);
        }
        loadScreen(screen, buffer, (int)size);
        free(buffer);
        writePng(screen, FileNameOut);
        return;
    }

    // A block was selected: pull the SCREEN$ out of the tap image.
    if (SelectedBlocks.count != 1 ||
        SelectedBlocks.block[0].start != SelectedBlocks.block[0].end)
    {
        fprintf(stderr, "Exactly one block should be selected\n");
        exit(1);
    }

    size_t size;
    byte *buffer = LoadFile(&size, FileNameIn);
    if (!buffer)
    {
        fprintf(stderr, "Unable to open file \"%s\"\n", FileNameIn);
        exit(1);
    }
    byte *endbuf = buffer + size;
    byte *pos = buffer;
    bool found = false;
    for (int i = 1; pos < endbuf; i++)
    {
        size_t blocksize = pos[0] | (pos[1] << 8);
        if (isSelected(i))
        {
            byte *payload;
            int len, avail;
            if (RawData)
            {
                payload = pos + 2;
                len = (int)blocksize;
                avail = (int)(endbuf - payload);
            } else {
                payload = pos + 3;
                len = blocksize >= 2 ? (int)blocksize - 2 : 0;
                avail = (int)(endbuf - payload);
            }
            if (avail < 0) avail = 0;
            if (len > avail) len = avail;
            loadScreen(screen, payload, len);
            found = true;
            break;
        }
        pos += blocksize + 2;
    }
    free(buffer);

    if (!found)
    {
        fprintf(stderr, "Selected block was not found\n");
        exit(1);
    }
    writePng(screen, FileNameOut);
}

//~~~~ .inf metadata + batch export ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~//

const char *typeName(int t)
{
    switch (t)
    {
        case DT_BASIC:     return "BASIC";
        case DT_NUMARRAY:  return "NUMARRAY";
        case DT_CHARARRAY: return "CHARARRAY";
        default:           return "CODE";
    }
}

int typeFromName(const char *s)
{
    if (!strcasecmp(s, "BASIC"))                              return DT_BASIC;
    if (!strcasecmp(s, "NUMARRAY") || !strcasecmp(s, "NUMBER"))    return DT_NUMARRAY;
    if (!strcasecmp(s, "CHARARRAY") || !strcasecmp(s, "CHAR"))     return DT_CHARARRAY;
    if (!strcasecmp(s, "CODE") || !strcasecmp(s, "BYTES"))    return DT_CODE;
    return -1;
}

// Make a filesystem-safe base name (<=10 chars) from a Spectrum block name.
void sanitizeName(char *dst, const char *src)
{
    int n = 0;
    for (int j = 0; j < 10; j++)
    {
        unsigned char c = (unsigned char)src[j];
        if (c == ' ' || c == '\0') continue;
        if (!isprint(c) || c == '/' || c == '\\' || c == ':') c = '_';
        dst[n++] = (char)c;
    }
    dst[n] = '\0';
}

void writeInfFile(const char *path, struct tapeheader *h)
{
    FILE *f = fopen(path, "w");
    if (!f)
    {
        fprintf(stderr, "W : cannot create \"%s\"\n", path);
        return;
    }
    int len   = h->HLenLo    | (h->HLenHi    << 8);
    int start = h->HStartLo  | (h->HStartHi  << 8);
    int p2    = h->HParam2Lo | (h->HParam2Hi << 8);
    char name[11];
    for (int j = 0; j < 10; j++)
        name[j] = isprint((unsigned char)h->HName[j]) ? h->HName[j] : ' ';
    name[10] = '\0';
    for (int e = 10; e > 0 && name[e - 1] == ' '; e--)
        name[e - 1] = '\0';

    fprintf(f, "File   : %s\n", name);
    fprintf(f, "type   : %s\n", typeName(h->HType));
    fprintf(f, "name   : %s\n", name);
    fprintf(f, "start  : %d\n", start);
    fprintf(f, "len    : %d\n", len);
    if (h->HType == DT_BASIC)
        fprintf(f, "vars   : %d\n", p2);
    fclose(f);
}

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t' || *s == '\r') s++;
    int e = (int)strlen(s);
    while (e > 0 && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r'))
        s[--e] = '\0';
    return s;
}

// Read a .inf file and populate the global TapeHeader for the next add/insert/replace.
void loadInf(const char *path)
{
    size_t sz;
    byte *buf = LoadFile(&sz, path);
    if (!buf)
    {
        fprintf(stderr, "Unable to open \"%s\"\n", path);
        exit(1);
    }
    buf[sz] = '\0';
    AddTapeHeader = true;

    char *line = (char*)buf;
    while (*line)
    {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        char *colon = strchr(line, ':');
        if (colon)
        {
            *colon = '\0';
            char *key = trim(line);
            char *val = trim(colon + 1);
            if (!strcasecmp(key, "type"))
            {
                int t = typeFromName(val);
                if (t >= 0) TapeHeader.HType = (byte)t;
            }
            else if (!strcasecmp(key, "name"))
            {
                int L = (int)strlen(val);
                for (int j = 0; j < 10; j++)
                    TapeHeader.HName[j] = (j < L) ? val[j] : ' ';
            }
            else if (!strcasecmp(key, "start"))
            {
                int a = atoi(val);
                TapeHeader.HStartLo = (byte)(a & 0xff);
                TapeHeader.HStartHi = (byte)(a >> 8);
            }
            else if (!strcasecmp(key, "vars"))
                BasicVarsParam = atoi(val);
        }
        if (!nl) break;
        line = nl + 1;
    }
    free(buf);
}

void cmdExportAll(const char *FileNameIn, const char *dirArg)
{
    const char *dir = (dirArg && dirArg[0]) ? dirArg : ".";
    size_t size;
    byte *buffer = LoadFile(&size, FileNameIn);
    if (!buffer)
    {
        fprintf(stderr, "Unable to open file \"%s\"\n", FileNameIn);
        exit(1);
    }
    if (dirArg && dirArg[0])
        MKDIR(dir);

    byte *endbuf = buffer + size;
    byte *pos = buffer;
    struct tapeheader *pending = NULL;     // last header block seen
    int fileCount = 0, infCount = 0;

    for (int i = 1; pos < endbuf; i++)
    {
        size_t blocksize = (pos + 1 < endbuf) ? (pos[0] | (pos[1] << 8)) : pos[0];
        bool isHeader = (blocksize == 19) && (pos + 2 < endbuf) && (pos[2] == 0x00);

        if (isHeader)
        {
            pending = (struct tapeheader*)pos;
            pos += blocksize + 2;
            continue;
        }

        // Anything else with content is treated as a data block -> one file.
        byte *payload;
        int len;
        if (RawData) { payload = pos + 2; len = (int)blocksize; }
        else         { payload = pos + 3; len = blocksize >= 2 ? (int)blocksize - 2 : 0; }
        int avail = (int)(endbuf - payload);
        if (avail < 0) avail = 0;
        if (len > avail) len = avail;
        if (len < 0) len = 0;

        char base[16] = "";
        const char *ext = "bin";
        if (pending)
        {
            sanitizeName(base, pending->HName);
            int st = pending->HStartLo | (pending->HStartHi << 8);
            if (pending->HType == DT_CODE && len == 6912 && st == 16384)
                ext = "scr";                       // a SCREEN$ -> export-png ready
        }

        char fname[MAX_PATH + 40];
        if (base[0])
            snprintf(fname, sizeof fname, "%s/%02d_%s.%s", dir, i, base, ext);
        else
            snprintf(fname, sizeof fname, "%s/%02d.%s", dir, i, ext);

        FILE *f = fopen(fname, "wb");
        if (!f)
        {
            fprintf(stderr, "Unable to create file \"%s\"\n", fname);
            free(buffer);
            exit(1);
        }
        if (len) writeBlock(payload, len, f, fname);
        fclose(f);
        fileCount++;
        if (!QuietMode)
            printf("o : %s (%d bytes)\n", fname, len);

        if (WriteInf && pending)
        {
            char infname[MAX_PATH + 48];
            snprintf(infname, sizeof infname, "%s.inf", fname);
            writeInfFile(infname, pending);
            infCount++;
        }
        pending = NULL;
        pos += blocksize + 2;
    }
    free(buffer);

    if (!QuietMode)
    {
        printf("O : %d file(s) exported to \"%s\"", fileCount, dir);
        if (WriteInf) printf(" (%d .inf)", infCount);
        printf("\n");
    }
}

void cmdInfo(const char *FileNameIn)
{
    size_t size;
    byte *buffer = LoadFile(&size, FileNameIn);
    if (!buffer)
    {
        fprintf(stderr, "Unable to open file \"%s\"\n", FileNameIn);
        exit(1);
    }
    byte *endbuf = buffer + size;
    byte *pos = buffer;
    int blocks = 0, headers = 0, datablk = 0, empty = 0, badcrc = 0, other = 0;
    long databytes = 0;
    bool corrupt = false;

    while (pos < endbuf)
    {
        size_t blocksize = (pos + 1 < endbuf) ? (pos[0] | (pos[1] << 8)) : pos[0];
        blocks++;
        if (blocksize == 0) { empty++; pos += 2; continue; }
        if ((pos + blocksize + 1) < endbuf)
        {
            if (blocksize > 1)
            {
                byte c = crc(&pos[2], blocksize - 1);
                if (pos[blocksize + 1] != c) badcrc++;
            }
            byte flag = pos[2];
            if (flag == 0x00 && blocksize == 19) headers++;
            else if (flag == 0xff) { datablk++; databytes += (long)blocksize - 2; }
            else other++;
        }
        else
            corrupt = true;
        pos += blocksize + 2;
    }
    if (pos != endbuf) corrupt = true;
    free(buffer);

    printf("o : File          : %s\n", FileNameIn);
    printf("o : Size          : %ld bytes\n", (long)size);
    printf("o : Blocks        : %d\n", blocks);
    printf("o : Headers       : %d\n", headers);
    printf("o : Data blocks   : %d\n", datablk);
    printf("o : Data bytes    : %ld\n", databytes);
    if (empty)  printf("o : Empty blocks  : %d\n", empty);
    if (other)  printf("o : Other blocks  : %d\n", other);
    if (badcrc) printf("W : Bad checksums : %d\n", badcrc);
    printf(corrupt ? "W : Integrity     : CORRUPTED\n"
                   : "O : Integrity     : OK\n");
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~//

int main(int argc, char **argv)
{
    char FileNameIn[MAX_PATH + 1] = "\0";
    char FileNameOut[MAX_PATH + 1] = "\0";
    bool AllOk = true;
    memset(&SelectedBlocks, 0, sizeof(SelectedBlocks));

    // parsing command-line arguments
    for (int i = 1; i < argc && AllOk; i++)
    {
        if (argv[i][0] == '-')
            switch (tolower(argv[i][1]))
            {
                case 'b':
                    AddTapeHeader = true;
                    TapeHeader.HType = DT_BASIC;
                    break;
                case 'h':
                    showUsage();
                    return 0;
                    break;
                case 'n':
                    if ((i + 1) < argc)
                    {
                        AddTapeHeader = true;
                        i++;
                        for(int j = 0; argv[i][j] != '\0' && j < 10; j++)
                            TapeHeader.HName[j] = argv[i][j];
                    } else {
                        fprintf(stderr, "Missing parameter");
                        AllOk = false;
                    }
                    break;
                case 'o':
                    if ((i + 1) < argc)
                    {
                        int adr = atoi(argv[i + 1]);
                        if (adr > 0xffff || adr < 0)
                        {
                            fprintf(stderr, "Origin address not in range 0-65535");
                            AllOk = false;
                        }
                        AddTapeHeader = true;
                        TapeHeader.HStartLo = (byte)(adr & 0xff);
                        TapeHeader.HStartHi = (byte)(adr >> 8);
                        i++;
                    } else {
                        fprintf(stderr, "Missing parameter");
                        AllOk = false;
                    }
                    break;
                case 'q':
                    QuietMode = true;
                    break;
                case 'r':
                    RawData = true;
                    break;
                case 's':
                    if ((i + 1) < argc)
                    {
                        int tmp = 0;
                        SelectedBlocks.count++;
                        for (char *c = argv[i + 1];
                             *c != '\0' && SelectedBlocks.count <= MAX_SELECTED_BLOCKS;
                             c++)
                        {
                            if (*c >= '0' && *c <= '9')
                                tmp = tmp * 10 + (*c - '0');
                            else if (*c == '-' && tmp != 0)
                            {
                                SelectedBlocks.block[SelectedBlocks.count - 1].start = tmp;
                                tmp = 0;
                            }
                            else if (*c == ',' && tmp != 0)
                            {
                                if (SelectedBlocks.block[SelectedBlocks.count - 1].start == 0)
                                    SelectedBlocks.block[SelectedBlocks.count - 1].start = tmp;
                                SelectedBlocks.block[SelectedBlocks.count - 1].end = tmp;
                                tmp = 0;
                                SelectedBlocks.count++;
                            }
                            else break;
                        }
                        if (tmp == 0)
                            SelectedBlocks.count--;
                        else
                        {
                            if (SelectedBlocks.block[SelectedBlocks.count - 1].start == 0)
                                SelectedBlocks.block[SelectedBlocks.count - 1].start = tmp;
                            SelectedBlocks.block[SelectedBlocks.count - 1].end = tmp;
                        }
                        i++;
                    } else {
                        fprintf(stderr, "Missing parameter");
                        AllOk = false;
                    }
                    break;
                case '-':
                    if (strcasecmp(&argv[i][2], "help") == 0)
                    {
                        showUsage();
                        return 0;
                    }
                    else if (strcasecmp(&argv[i][2], "raw") == 0)
                        RawData = true;
                    else if (strcasecmp(&argv[i][2], "inf") == 0)
                        WriteInf = true;
                    else if (strcasecmp(&argv[i][2], "from-inf") == 0)
                    {
                        if ((i + 1) < argc)
                        {
                            InfPath = argv[i + 1];
                            i++;
                        } else {
                            fprintf(stderr, "Missing parameter");
                            AllOk = false;
                        }
                    }
                    else {
                        fprintf(stderr, "Unknown option");
                        AllOk = false;
                    }
                    break;
                default:
                    fprintf(stderr, "Unknown option");
                    AllOk = false;
                    break;
            }
        else if (!Command)
        {
            if (strcasecmp(argv[i], "add") == 0)
                Command = cm_Add;
            else if (strcasecmp(argv[i], "insert") == 0)
                Command = cm_Insert;
            else if (strcasecmp(argv[i], "extract") == 0)
                Command = cm_Extract;
            else if (strcasecmp(argv[i], "list") == 0)
                Command = cm_List;
            else if (strcasecmp(argv[i], "remove") == 0)
                Command = cm_Remove;
            else if (strcasecmp(argv[i], "replace") == 0)
                Command = cm_Replace;
            else if (strcasecmp(argv[i], "fix-0") == 0)
                Command = cm_Fix0;
            else if (strcasecmp(argv[i], "fix-crc") == 0)
                Command = cm_FixCrc;
            else if (strcasecmp(argv[i], "export-png") == 0)
                Command = cm_ExportPng;
            else if (strcasecmp(argv[i], "export-all") == 0)
                Command = cm_ExportAll;
            else if (strcasecmp(argv[i], "info") == 0)
                Command = cm_Info;
        }
        else if (FileNameIn[0] == '\0')
            strncpy(FileNameIn, argv[i], MAX_PATH);
        else if (FileNameOut[0] == '\0')
            strncpy(FileNameOut, argv[i], MAX_PATH);
    }

    if (!Command && AllOk)
    {
        fprintf(stderr, "Unknown command");
        AllOk = false;
    }

    if (FileNameIn[0] == '\0' && AllOk)
    {
        fprintf(stderr, "No input file name was provided");
        AllOk = false;
    }

    if (!AllOk)
    {
        fprintf(stderr, ", try --help\n");
        exit(1);
    }

    if (InfPath)
        loadInf(InfPath);

    switch (Command)
    {
        case cm_Add:
            chkOutFile(FileNameOut);
            cmdAdd(FileNameIn, FileNameOut);
            break;
        case cm_Insert:
            chkOutFile(FileNameOut);
            cmdInsert(FileNameIn, FileNameOut);
            break;
        case cm_Extract:
            chkOutFile(FileNameOut);
            cmdExtract(FileNameIn, FileNameOut);
            break;
        case cm_Remove:
            cmdRemove(FileNameIn);
            break;
        case cm_Replace:
            chkOutFile(FileNameOut);
            cmdReplace(FileNameIn, FileNameOut);
            break;
        case cm_List:
            cmdList(FileNameIn);
            break;
        case cm_Fix0:
            cmdFix0(FileNameIn);
            break;
        case cm_FixCrc:
            cmdFixCrc(FileNameIn);
            break;
        case cm_ExportPng:
            chkOutFile(FileNameOut);
            cmdExportPng(FileNameIn, FileNameOut);
            break;
        case cm_ExportAll:
            cmdExportAll(FileNameIn, FileNameOut);
            break;
        case cm_Info:
            cmdInfo(FileNameIn);
            break;
        default:
            break;
    }

    return 0;
}

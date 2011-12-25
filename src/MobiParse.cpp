/* Copyright 2011-2012 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

#include "MobiParse.h"
#include "FileUtil.h"
#include "StrUtil.h"

// Parse mobi format http://wiki.mobileread.com/wiki/MOBI

#define DETAILED_LOGGING 1 // set to 1 for detailed logging during debugging
#if DETAILED_LOGGING
#define l(s) printf(s)
#else
#define l(s) NoOp()
#endif

#define PALMDOC_TYPE_CREATOR   "TEXtREAd"
#define MOBI_TYPE_CREATOR      "BOOKMOBI"


#define COMPRESSION_NONE 1
#define COMPRESSION_PALM 2
#define COMPRESSION_HUFF 17480

#define ENCRYPTION_NONE 0
#define ENCRYPTION_OLD  1
#define ENCRYPTION_NEW  2

// http://wiki.mobileread.com/wiki/MOBI#PalmDOC_Header
#define kPalmDocHeaderLen 16
struct PalmDocHeader
{
    int16_t    compressionType;
    int16_t    reserved1;
    int32_t    uncompressedDocSize;
    int16_t    recordsCount;
    int16_t    maxRecSize;     // usually (always?) 4096
    // if it's palmdoc, we have currPos, if mobi, encrType/reserved2
    union {
        int32_t    currPos;
        struct {
          int16_t  encrType;
          int16_t  reserved2;
        } mobi;
    };
};
STATIC_ASSERT(kPalmDocHeaderLen == sizeof(PalmDocHeader), validMobiFirstRecord);

enum MobiDocType {
    TypeMobiDoc = 2,
    TypePalmDoc = 3,
    TypeAudio = 4,
    TypeNews = 257,
    TypeNewsFeed = 258,
    TypeNewsMagazin = 259,
    TypePics = 513,
    TypeWord = 514,
    TypeXls = 515,
    TypePpt = 516,
    TypeText = 517,
    TyepHtml = 518
};

// http://wiki.mobileread.com/wiki/MOBI#MOBI_Header
// Note: the real length of MobiHeader is in MobiHeader.hdrLen. This is just
// the size of the struct
#define kMobiHeaderLen 232
struct MobiHeader {
    char        id[4];
    uint32_t     hdrLen;   // including 4 id bytes
    uint32_t     type;     // MobiDocType
    uint32_t     textEncoding;
    uint32_t     uniqueId;
    uint32_t     mobiFormatVersion;
    uint32_t     ortographicIdxRec; // -1 if no ortographics index
    uint32_t     inflectionIdxRec;
    uint32_t     namesIdxRec;
    uint32_t     keysIdxRec;
    uint32_t     extraIdx0Rec;
    uint32_t     extraIdx1Rec;
    uint32_t     extraIdx2Rec;
    uint32_t     extraIdx3Rec;
    uint32_t     extraIdx4Rec;
    uint32_t     extraIdx5Rec;
    uint32_t     firstNonBookRec;
    uint32_t     fullNameOffset; // offset in record 0
    uint32_t     fullNameLen;
    // Low byte is main language e.g. 09 = English, 
    // next byte is dialect, 08 = British, 04 = US. 
    // Thus US English is 1033, UK English is 2057
    uint32_t     locale;
    uint32_t     inputDictLanguage;
    uint32_t     outputDictLanguage;
    uint32_t     minRequiredMobiFormatVersion;
    uint32_t     imageFirstRec;
    uint32_t     huffmanFirstRec;
    uint32_t     huffmanRecCount;
    uint32_t     huffmanTableOffset;
    uint32_t     huffmanTableLen;
    uint32_t     exhtFlags;  // bitfield. if bit 6 (0x40) is set => there's an EXTH record
    char        reserved1[32];
    uint32_t     drmOffset; // -1 if no drm info
    uint32_t     drmEntriesCount; // -1 if no drm
    uint32_t     drmSize;
    uint32_t     drmFlags;
    char        reserved2[62];
    // A set of binary flags, some of which indicate extra data at the end of each text block.
    // This only seems to be valid for Mobipocket format version 5 and 6 (and higher?), when
    // the header length is 228 (0xE4) or 232 (0xE8).
    uint16_t    extraDataFlags;
    int32_t     indxRec;
};

STATIC_ASSERT(kMobiHeaderLen == sizeof(MobiHeader), validMobiHeader);

// change big-endian int16 to little-endian (our native format)
static void SwapI16(int16_t& i)
{
    i = BEtoHs(i);
}

static void SwapU16(uint16_t& i)
{
    i = BEtoHs(i);
}

// change big-endian int32 to little-endian (our native format)
static void SwapI32(int32_t& i)
{
    i = BEtoHl(i);
}

static void SwapU32(uint32_t& i)
{
    i = BEtoHl(i);
}

// Uncompress source data compressed with PalmDoc compression into a buffer.
// Returns size of uncompressed data or -1 on error (if destination buffer too small)
static size_t PalmdocUncompress(uint8_t *src, size_t srcLen, uint8_t *dst, size_t dstLen)
{
    uint8_t *srcEnd = src + srcLen;
    uint8_t *dstEnd = dst + dstLen;
    uint8_t *dstOrig = dst;
    size_t dstLeft;
    while (src < srcEnd) {
        dstLeft = dstEnd - dst;
        assert(dstLeft > 0);
        if (0 == dstLeft)
            return -1;

        unsigned c = *src++;

        if ((c >= 1) && (c <= 8)) {
            assert(dstLeft >= c);
            if (dstLeft < c)
                return -1;
            while (c > 0) {
                *dst++ = *src++;
                --c;
            }
        } else if (c < 128) {
            assert(c != 0);
            *dst++ = c;
        } else if (c >= 192) {
            assert(dstLeft >= 2);
            if (dstLeft < 2)
                return -1;
            *dst++ = ' ';
            *dst++ = c ^ 0x80;
        } else {
            assert((c >= 128) && (c < 192));
            assert(src < srcEnd);
            if (src < srcEnd) {
                c = (c << 8) | *src++;
                size_t back = (c >> 3) & 0x07ff;
                size_t n = (c & 7) + 3;
                uint8_t *dstBack = dst - back;
                assert(dstBack >= dstOrig);
                assert(dstLeft >= n);
                while (n > 0) {
                    *dst++ = *dstBack++;
                    --n;
                }
            }
        }
    }

    // zero-terminate to make inspecting in the debugger easier
    if (dst < dstEnd)
        *dst = 0;

    return dst - dstOrig;
}

#define kHuffHeaderLen 24
struct HuffHeader
{
    char         id[4];           // �HUFF�
    uint32_t     hdrLen;          // should be 24
    uint32_t     cacheOffset;     // should be 24 as well
    uint32_t     baseTableOffset; // 1024
    char         unknown[8];
};

STATIC_ASSERT(kHuffHeaderLen == sizeof(HuffHeader), validHuffHeader);

struct Dict1Entry {
    uint32_t    codeLen;
    uint32_t    term;
    uint32_t    maxCode;
};

struct Dic2Entry {
};

class HuffDicDecompressor 
{
    // underlying data for cache and baseTable
    // (an optimization to only do one allocation instead of two)
    uint8_t *   huffmanData;

    uint8_t *   baseTable;
    size_t      baseTableLen;

    Dict1Entry  dict1[256];
    uint32_t    minCode;
    uint32_t    maxCode;

    Vec<Dic2Entry> dictionary;

public:
    HuffDicDecompressor();
    ~HuffDicDecompressor();
    bool SetHuffData(uint8_t *huffData, size_t huffDataLen);
    bool UnpackCacheData(uint32_t *cache);
    bool AddCdicData(uint8_t *cdicData, size_t cdicDataLen);
};

HuffDicDecompressor::HuffDicDecompressor() :
    huffmanData(NULL), baseTable(NULL)
{
}

HuffDicDecompressor::~HuffDicDecompressor()
{
    free(huffmanData);
}

// always 256 values in cache, caller ensures that
bool HuffDicDecompressor::UnpackCacheData(uint32_t *cache)
{
    for (size_t i = 0; i < 256; i++) {
        uint32_t v = cache[i];
        SwapU32(v);
        dict1[i].codeLen = v & 0x1f;
        if (0 == dict1[i].codeLen)
            return false;
        dict1[i].term    = v & 0x80;
        if ((dict1[i].codeLen <= 8) && (0 == dict1[i].term))
            return false;
        uint32_t maxCode = v >> 8;
        maxCode = ((maxCode + 1) << (32 - dict1[i].codeLen)) - 1;
        dict1[i].maxCode = maxCode;
    }
    return true;
}

bool HuffDicDecompressor::SetHuffData(uint8_t *huffData, size_t huffDataLen)
{
    assert(huffDataLen > kHuffHeaderLen);
    if (huffDataLen < kHuffHeaderLen)
        return false;
    HuffHeader *huffHdr = (HuffHeader*)huffData;
    SwapU32(huffHdr->hdrLen);
    SwapU32(huffHdr->cacheOffset);
    SwapU32(huffHdr->baseTableOffset);

    if (!str::EqN("HUFF", huffHdr->id, 4))
        return false;
    assert(huffHdr->hdrLen == kHuffHeaderLen);
    if (huffHdr->hdrLen != kHuffHeaderLen)
        return false;
    if (huffHdr->baseTableOffset != (huffHdr->cacheOffset + 1024))
        return false;
    if (huffHdr->baseTableOffset >= huffDataLen)
        return false;
    // could skip huffData's HuffHeader, by why complicate code?
    assert(NULL == huffmanData);
    huffmanData = (uint8_t*)memdup(huffData, huffDataLen);
    if (!huffmanData)
        return false;
    uint32_t *cache = (uint32_t*)(huffmanData + huffHdr->cacheOffset);
    UnpackCacheData(cache);

    baseTable = (uint8_t*)(huffmanData + huffHdr->baseTableOffset);
    baseTableLen = huffDataLen - huffHdr->baseTableOffset;
    return true;
}

bool HuffDicDecompressor::AddCdicData(uint8_t *cdicData, size_t cdicDataLen)
{
    return false;
}

static bool IsMobiPdb(PdbHeader *pdbHdr)
{
    return str::EqN(pdbHdr->type, MOBI_TYPE_CREATOR, 8);
}

static bool IsPalmDocPdb(PdbHeader *pdbHdr)
{
    return str::EqN(pdbHdr->type, PALMDOC_TYPE_CREATOR, 8);
}

static bool IsValidCompression(int comprType)
{
    return  (COMPRESSION_NONE == comprType) ||
            (COMPRESSION_PALM == comprType) ||
            (COMPRESSION_HUFF == comprType);
}

MobiParse::MobiParse() : 
    fileName(NULL), fileHandle(0), recHeaders(NULL), firstRecData(NULL), isMobi(false),
    docRecCount(0), compressionType(0), docUncompressedSize(0),
    multibyte(false), trailersCount(0), bufDynamic(NULL), bufDynamicSize(0),
    huffDic(NULL)
{
}

MobiParse::~MobiParse()
{
    CloseHandle(fileHandle);
    free(fileName);
    free(firstRecData);
    free(recHeaders);
    free(bufDynamic);
    delete huffDic;
}

bool MobiParse::ParseHeader()
{
    DWORD bytesRead;
    BOOL ok = ReadFile(fileHandle, (void*)&pdbHeader, kPdbHeaderLen, &bytesRead, NULL);
    if (!ok || (kPdbHeaderLen != bytesRead))
        return false;

    if (IsMobiPdb(&pdbHeader)) {
        isMobi = true;
    } else if (IsPalmDocPdb(&pdbHeader)) {
        isMobi = false;
    } else {
        // TODO: print type/creator
        l(" unknown pdb type/creator\n");
        return false;
    }

    // the values are in big-endian, so convert to host order
    // but only those that we actually access
    SwapI16(pdbHeader.numRecords);
    if (pdbHeader.numRecords < 1)
        return false;

    // allocate one more record as a sentinel to make calculating
    // size of the records easier
    recHeaders = SAZA(PdbRecordHeader, pdbHeader.numRecords + 1);
    DWORD toRead = kPdbRecordHeaderLen * pdbHeader.numRecords;
    ok = ReadFile(fileHandle, (void*)recHeaders, toRead, &bytesRead, NULL);
    if (!ok || (toRead != bytesRead)) {
        return false;
    }
    for (int i = 0; i < pdbHeader.numRecords; i++) {
        SwapI32(recHeaders[i].offset);
    }
    size_t fileSize = file::GetSize(fileName);
    recHeaders[pdbHeader.numRecords].offset = fileSize;
    // validate offsets
    for (int i = 0; i < pdbHeader.numRecords; i++) {
        if (recHeaders[i + 1].offset < recHeaders[i].offset) {
            l("invalid offset field\n");
            return false;
        }
        // technically PDB record size should be less than 64K,
        // but it's not true for mobi files, so we don't validate that
    }

    size_t recLeft;
    char *buf = ReadRecord(0, recLeft);
    if (NULL == buf) {
        l("failed to read record\n");
        return false;
    }

    assert(NULL == firstRecData);
    firstRecData = (char*)memdup(buf, recLeft);
    if (!firstRecData)
        return false;
    char *currRecPos = firstRecData;
    PalmDocHeader *palmDocHdr = (PalmDocHeader*)currRecPos;
    currRecPos += sizeof(PalmDocHeader);
    recLeft -= sizeof(PalmDocHeader);

    SwapI16(palmDocHdr->compressionType);
    SwapI32(palmDocHdr->uncompressedDocSize);
    SwapI16(palmDocHdr->recordsCount);
    //SwapI16(palmDocHdr->maxUncompressedRecSize);
    if (!IsValidCompression(palmDocHdr->compressionType)) {
        l("unknown compression type\n");
        return false;
    }
    if (isMobi) {
        // TODO: this needs to be surfaced to the client so
        // that we can show the right error message
        if (palmDocHdr->mobi.encrType != ENCRYPTION_NONE) {
            l("encryption is unsupported\n");
            return false;
        }
    }

    docRecCount = palmDocHdr->recordsCount;
    docUncompressedSize = palmDocHdr->uncompressedDocSize;
    compressionType = palmDocHdr->compressionType;

    if (0 == recLeft) {
        assert(!isMobi);
        return true;
    }
    if (recLeft < 8) // id and hdrLen
        return false;
    MobiHeader *mobiHdr = (MobiHeader*)currRecPos;
    if (!str::EqN("MOBI", mobiHdr->id, 4)) {
        l("MobiHeader.id is not 'MOBI'\n");
        return false;
    }
    SwapU32(mobiHdr->hdrLen);
    SwapU32(mobiHdr->type);
    SwapU32(mobiHdr->textEncoding);
    SwapU32(mobiHdr->mobiFormatVersion);
    SwapU32(mobiHdr->firstNonBookRec);
    SwapU32(mobiHdr->fullNameOffset);
    SwapU32(mobiHdr->fullNameLen);
    SwapU32(mobiHdr->locale);
    SwapU32(mobiHdr->minRequiredMobiFormatVersion);
    SwapU32(mobiHdr->imageFirstRec);
    SwapU32(mobiHdr->huffmanFirstRec);
    SwapU32(mobiHdr->huffmanRecCount);
    SwapU32(mobiHdr->huffmanTableOffset);
    SwapU32(mobiHdr->huffmanTableLen);
    SwapU32(mobiHdr->exhtFlags);

    size_t hdrLen = mobiHdr->hdrLen;
    if (hdrLen > recLeft) {
        l("MobiHeader too big\n");
        return false;
    }
    currRecPos += hdrLen;
    recLeft -= hdrLen;
    bool hasExtraFlags = (hdrLen >= 228); // TODO: also only if mobiFormatVersion >= 5?

    if (hasExtraFlags) {
        SwapU16(mobiHdr->extraDataFlags);
        uint16_t flags = mobiHdr->extraDataFlags;
        multibyte = ((flags & 1) != 0);
        while (flags > 1) {
            if (0 != (flags & 2))
                trailersCount++;
            flags = flags >> 1;
        }
    }

    if (palmDocHdr->compressionType == COMPRESSION_HUFF) {
        assert(isMobi);
        size_t recSize;
        char *recData = ReadRecord(mobiHdr->huffmanFirstRec, recSize);
        if (!recData)
            return false;
        assert(NULL == huffDic);
        huffDic = new HuffDicDecompressor();
        if (!huffDic->SetHuffData((uint8_t*)recData, recSize))
            return false;
        for (size_t i = 1; i < mobiHdr->huffmanRecCount; i++) {
            char *recData = ReadRecord(mobiHdr->huffmanFirstRec + i, recSize);
            if (!recData)
                return false;
            if (!huffDic->AddCdicData((uint8_t*)recData, recSize))
                return false;
        }
    }

    return true;
}

size_t MobiParse::GetRecordSize(size_t recNo)
{
    size_t size = recHeaders[recNo + 1].offset - recHeaders[recNo].offset;
    return size;
}

// returns NULL if error (failed to allocated)
char *MobiParse::GetBufForRecordData(size_t size)
{
    if (size <= sizeof(bufStatic))
        return bufStatic;
    if (size <= bufDynamicSize)
        return bufDynamic;
    free(bufDynamic);
    bufDynamic = (char*)malloc(size);
    bufDynamicSize = size;
    return bufDynamic;
}

// read a record and return it's data and size. Return NULL if error
char* MobiParse::ReadRecord(size_t recNo, size_t& sizeOut)
{
    size_t off = recHeaders[recNo].offset;
    DWORD toRead = GetRecordSize(recNo);
    sizeOut = toRead;
    char *buf = GetBufForRecordData(toRead);
    if (NULL == buf)
        return NULL;
    DWORD bytesRead;
    DWORD res = SetFilePointer(fileHandle, off, NULL, FILE_BEGIN);
    if (INVALID_SET_FILE_POINTER == res)
        return NULL;
    BOOL ok = ReadFile(fileHandle, (void*)buf, toRead, &bytesRead, NULL);
    if (!ok || (toRead != bytesRead))
        return NULL;
    return buf;
}

// each record can have extra data at the end, which we must discard
static size_t ExtraDataSize(uint8_t *recData, size_t recLen, size_t trailersCount, bool multibyte)
{
    size_t newLen = recLen;
    for (size_t i = 0; i < trailersCount; i++) {
        assert(newLen > 4);
        uint32_t n = 0;
        for (size_t j = 0; j < 4; j++) {
            uint8_t v = recData[newLen - 4 + j];
            if (0 != (v & 0x80))
                n = 0;
            n = (n << 7) | (v & 0x7f);
        }
        assert(newLen > n);
        newLen -= n;
    }

    if (multibyte) {
        assert(newLen > 0);
        if (newLen > 0) {
            uint8_t n = (recData[newLen-1] & 3) + 1;
            assert(newLen >= n);
            newLen -= n;
        }
    }
    return recLen - newLen;
}

// Load a given record of a document into strOut, uncompressing if necessary.
// Returns false if error.
bool MobiParse::LoadDocRecordIntoBuffer(size_t recNo, str::Str<char>& strOut)
{
    size_t recSize;
    char *recData = ReadRecord(recNo, recSize);
    if (NULL == recData)
        return false;
    size_t extraSize = ExtraDataSize((uint8_t*)recData, recSize, trailersCount, multibyte);
    recSize -= extraSize;
    if (COMPRESSION_NONE == compressionType) {
        strOut.Append(recData, recSize);
        return true;
    }

    if (COMPRESSION_PALM == compressionType) {
        char buf[6000]; // should be enough to decompress any record
        size_t uncompressedSize = PalmdocUncompress((uint8_t*)recData, recSize, (uint8_t*)buf, sizeof(buf));
        if (-1 == uncompressedSize) {
            l("PalmDoc decompression failed\n");
            return false;
        }
        strOut.Append(buf, uncompressedSize);
        return true;
    }

    if (COMPRESSION_HUFF == compressionType) {
        // TODO: implement me
        return false;
    }

    assert(0);
    return false;
}

// assumes that ParseHeader() has been called
bool MobiParse::LoadDocument()
{
    assert(docUncompressedSize > 0);
    assert(0 == doc.Size());
    for (size_t i = 1; i <= docRecCount; i++) {
        if (!LoadDocRecordIntoBuffer(i, doc))
            return false;
    }
    assert(docUncompressedSize == doc.Size());
    return true;
}

MobiParse *MobiParse::ParseFile(const TCHAR *fileName)
{
    HANDLE fh = CreateFile(fileName, GENERIC_READ, FILE_SHARE_READ, NULL,  
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (fh == INVALID_HANDLE_VALUE)
        return NULL;
    MobiParse *mb = new MobiParse();
    mb->fileName = str::Dup(fileName);
    mb->fileHandle = fh;
    if (!mb->ParseHeader())
        goto Error;
    if (!mb->LoadDocument())
        goto Error;
    return mb;
Error:
    delete mb;
    return NULL;
}
// ============================================================================
// AddFileInfo.cpp
//
// Functional rewrite of the "AddFileInfo" tool, based on reverse engineering
// of the Ghidra decompilation output (AddFileInfo.c, ~9500 lines of
// decompiled Win32/MSVC code).
//
// NOTE - WHAT THIS IS, AND WHAT IT IS NOT:
//   This file is NOT a mechanical translation of every instruction from the
//   decompilation (most of which is statically linked MSVC runtime: SEH
//   exception handling, its own allocator, file descriptor tables, an
//   internal buffered file stream class, and CRLF/LF conversion). Instead,
//   this is a reimplementation in idiomatic, modern C++ that reproduces the
//   recognized behavior of the program:
//
//     1) command line argument parsing,
//     2) GUID generation / parsing,
//     3) writing a binary metadata header to the destination file,
//     4) copying the contents of the source file after that header,
//        with optional CRLF -> LF conversion in text mode.
//
// HEADER FORMAT (reconstructed from the header-writing function and from
// the function that reads/verifies it later in the original code):
//
//   uint32_t magicAndFlags;
//       - lower 24 bits (0x00FFFFFF) = magic constant 0x00D0A1FF
//       - bit 27 (0x08000000) set <=> "name" section present
//       - bit 28 (0x10000000) set <=> "attrib" section present
//       - bit 29 (0x20000000) set <=> "guid" section present
//
//   [ if the name bit is set ]
//   uint8_t  nameLen;
//   char     name[nameLen];       // no NUL terminator in the file
//
//   [ if the attrib bit is set ]
//   uint32_t attrib;
//
//   [ if the guid bit is set ]
//   uint8_t  guid[16];            // raw 16 bytes of the GUID (Data1..Data4)
//
//   [ then ]
//   <the rest of the source file's contents, copied 1:1 or with
//    CRLF -> LF conversion, depending on the mode>
//
// INVOCATION SYNTAX (reconstructed from the argv-parsing loop in the
// original):
//
//   AddFileInfo <source_file> <dest_file> [options...]
//
//   options:
//     -text                  text mode when reading the source (CRLF -> LF)
//     -binary                binary mode when reading the source (default)
//     -name <name>            append a section with a name (max 255 chars)
//     -guid <GUID>             use the given GUID, format:
//                              XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX
//     -guidCreate             generate a new random GUID (CoCreateGuid)
//     -guidNone               do not include a GUID section (default behavior)
//     -attrib <value>         append a 32-bit attributes value
//                              (decimal or with 0x prefix)
//
// The program is for Windows (uses the Win32 API: CreateFileA, CoCreateGuid),
// just like the original .exe file.
// ============================================================================

#include <windows.h>
#include <objbase.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <memory>

#pragma comment(lib, "ole32.lib")

namespace {

constexpr uint32_t kMagicBase   = 0x00D0A1FF;
constexpr uint32_t kNameFlagBit = 0x08000000;
constexpr uint32_t kAttribFlagBit = 0x10000000;
constexpr uint32_t kGuidFlagBit = 0x20000000;

// ---------------------------------------------------------------------
// Simple wrapper around a Win32 file handle, so RAII closes it
// automatically.
// ---------------------------------------------------------------------
class Win32File {
public:
    Win32File() = default;
    ~Win32File() { close(); }

    Win32File(const Win32File&) = delete;
    Win32File& operator=(const Win32File&) = delete;

    bool openForRead(const std::string& path) {
        handle_ = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        return handle_ != INVALID_HANDLE_VALUE;
    }

    bool openForWrite(const std::string& path) {
        handle_ = CreateFileA(path.c_str(), GENERIC_WRITE, 0,
                              nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        return handle_ != INVALID_HANDLE_VALUE;
    }

    bool readByte(uint8_t& out) {
        DWORD read = 0;
        BOOL ok = ReadFile(handle_, &out, 1, &read, nullptr);
        return ok && read == 1;
    }

    // Reads exactly 'len' bytes into buffer 'buf' (used when reading
    // multi-byte header fields: length+name, attrib, guid).
    bool readBytes(void* buf, size_t len) {
        if (len == 0) return true;
        DWORD read = 0;
        BOOL ok = ReadFile(handle_, buf, static_cast<DWORD>(len), &read, nullptr);
        return ok && read == len;
    }

    bool writeBytes(const void* data, size_t len) {
        DWORD written = 0;
        BOOL ok = WriteFile(handle_, data, static_cast<DWORD>(len), &written, nullptr);
        return ok && written == len;
    }

    void close() {
        if (handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
        }
    }

    bool isOpen() const { return handle_ != INVALID_HANDLE_VALUE; }

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};

// ---------------------------------------------------------------------
// Command line options
// ---------------------------------------------------------------------
struct Options {
    // Encode = normal operation (append header + copy file).
    // Decode = reverse "-extract" mode: read the header and write the
    //          source contents back out to a plain file.
    enum class Mode { Encode, Decode };
    Mode mode = Mode::Encode;

    std::string sourcePath;
    std::string destPath;

    bool textMode = false;      // -text / -binary (binary is the default)

    // Decode mode only: whether to re-insert a CR before each LF
    // (reconstructing CRLF). See the comment on exportBody().
    bool expandCrLf = false;

    bool hasName = false;       // -name <name>
    std::string name;

    bool hasAttrib = false;     // -attrib <value>
    uint32_t attrib = 0;

    enum class GuidMode { None, Explicit, Create } guidMode = GuidMode::None;
    GUID guid{};                // populated for GuidMode::Explicit / Create
};

bool iequals(const char* a, const char* b) {
    return _stricmp(a, b) == 0;
}

// Parses a GUID in the standard text format (without curly braces),
// the same way the original did it (sscanf with format "%08x-%04x-%04x-...").
bool parseGuid(const char* text, GUID& out) {
    unsigned long data1 = 0;
    unsigned int data2 = 0, data3 = 0;
    unsigned int d[8] = {0};

    int n = std::sscanf(text,
                        "%8lx-%4x-%4x-%2x%2x-%2x%2x%2x%2x%2x%2x",
                        &data1, &data2, &data3,
                        &d[0], &d[1], &d[2], &d[3], &d[4], &d[5], &d[6], &d[7]);

    if (n != 11) {
        return false;
    }

    out.Data1 = static_cast<unsigned long>(data1);
    out.Data2 = static_cast<unsigned short>(data2);
    out.Data3 = static_cast<unsigned short>(data3);
    for (int i = 0; i < 8; ++i) {
        out.Data4[i] = static_cast<unsigned char>(d[i]);
    }
    return true;
}

void printUsage() {
    std::fprintf(stderr,
                 "Usage: AddFileInfo <source_file> <dest_file> [options]\n"
                 "\n"
                 "Options:\n"
                 "  -text                text mode when reading the source (CRLF -> LF)\n"
                 "  -binary              binary mode when reading the source (default)\n"
                 "  -name <name>         append a section with a name\n"
                 "  -guid <GUID>         use the given GUID\n"
                 "                       (format: XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX)\n"
                 "  -guidCreate          generate a new random GUID\n"
                 "  -guidNone            do not include a GUID section (default)\n"
                 "  -attrib <value>      append an attributes value (decimal or 0x..)\n"
                 "\n"
                 "Reverse mode (reads a file already processed by AddFileInfo):\n"
                 "  AddFileInfo -extract <binary_file> <output_file> [-crlf]\n"
                 "\n"
                 "    Prints the header metadata (name/attrib/guid) to stdout\n"
                 "    and writes the rest of the file (source content) to <output_file>.\n"
                 "    -crlf   inserts a CR before every LF (reconstructing CRLF); without\n"
                 "            this option the content is written exactly as it lies\n"
                 "            in the container (see the note on -text's lossiness in the code).\n");
}

// Returns false on a parsing error (and prints a message).
bool parseArgs(int argc, char** argv, Options& opts) {
    // Reverse mode: AddFileInfo -extract <binary_file> <output_file> [-crlf]
    if (argc >= 2 && iequals(argv[1], "-extract")) {
        opts.mode = Options::Mode::Decode;

        if (argc < 4) {
            std::fprintf(stderr,
                         "Error: syntax: AddFileInfo -extract <binary_file> "
                         "<output_file> [-crlf]\n");
            return false;
        }

        opts.sourcePath = argv[2];
        opts.destPath   = argv[3];

        for (int i = 4; i < argc; ++i) {
            if (iequals(argv[i], "-crlf")) {
                opts.expandCrLf = true;
            } else {
                std::fprintf(stderr, "Error: unknown option in -extract mode: %s\n", argv[i]);
                return false;
            }
        }

        return true;
    }

    if (argc < 3) {
        printUsage();
        return false;
    }

    opts.sourcePath = argv[1];
    opts.destPath   = argv[2];

    for (int i = 3; i < argc; ++i) {
        const char* arg = argv[i];

        if (iequals(arg, "-text")) {
            opts.textMode = true;
        } else if (iequals(arg, "-binary")) {
            opts.textMode = false;
        } else if (iequals(arg, "-guidCreate")) {
            opts.guidMode = Options::GuidMode::Create;
        } else if (iequals(arg, "-guidNone")) {
            opts.guidMode = Options::GuidMode::None;
        } else if (iequals(arg, "-name")) {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "Error: missing value for -name\n");
                return false;
            }
            opts.name = argv[++i];
            if (opts.name.size() > 255) {
                std::fprintf(stderr, "Error: name is too long (max 255 characters)\n");
                return false;
            }
            opts.hasName = !opts.name.empty();
        } else if (iequals(arg, "-guid")) {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "Error: missing value for -guid\n");
                return false;
            }
            if (!parseGuid(argv[++i], opts.guid)) {
                std::fprintf(stderr, "Error: invalid GUID format: %s\n", argv[i]);
                return false;
            }
            opts.guidMode = Options::GuidMode::Explicit;
        } else if (iequals(arg, "-attrib")) {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "Error: missing value for -attrib\n");
                return false;
            }
            char* end = nullptr;
            unsigned long v = std::strtoul(argv[++i], &end, 0);
            if (end == argv[i] || *end != '\0') {
                std::fprintf(stderr, "Error: invalid value for -attrib: %s\n", argv[i]);
                return false;
            }
            opts.attrib = static_cast<uint32_t>(v);
            opts.hasAttrib = (opts.attrib != 0);
        } else {
            std::fprintf(stderr, "Error: unknown option: %s\n", arg);
            printUsage();
            return false;
        }
    }

    return true;
}

// Builds and writes the metadata header to the already-open destination file.
bool writeHeader(Win32File& dest, const Options& opts) {
    const bool hasGuid = (opts.guidMode != Options::GuidMode::None);

    uint32_t flagNibble = 0;
    if (opts.hasName)   flagNibble |= 0x08;
    if (opts.hasAttrib) flagNibble |= 0x10;
    if (hasGuid)         flagNibble |= 0x20;

    // Verified byte-by-byte against an InterfaceJS_copy.def/.int sample:
    // the flags sit in bits 27-29 (0x08000000/0x10000000/0x20000000),
    // so flagNibble (0x08/0x10/0x20) is shifted by 24, not by 20.
    uint32_t magicAndFlags = (flagNibble << 24) | kMagicBase;

    if (!dest.writeBytes(&magicAndFlags, sizeof(magicAndFlags))) {
        return false;
    }

    if (opts.hasName) {
        uint8_t len = static_cast<uint8_t>(opts.name.size());
        if (!dest.writeBytes(&len, sizeof(len))) return false;
        if (!dest.writeBytes(opts.name.data(), opts.name.size())) return false;
    }

    if (opts.hasAttrib) {
        if (!dest.writeBytes(&opts.attrib, sizeof(opts.attrib))) return false;
    }

    if (hasGuid) {
        // Write the raw 16 bytes of the GUID structure (Data1..Data4),
        // exactly as the original did.
        if (!dest.writeBytes(&opts.guid, sizeof(GUID))) return false;
    }

    return true;
}

// Copies the source file's contents to the destination, optionally
// converting CRLF -> LF in text mode (as the original did with -text).
bool copyBody(Win32File& src, Win32File& dest, bool textMode) {
    uint8_t byte = 0;
    uint8_t prev = 0;
    bool havePrev = false;

    while (src.readByte(byte)) {
        if (textMode) {
            if (havePrev) {
                if (prev == '\r' && byte == '\n') {
                    // CRLF -> LF: skip the CR, only write the LF below.
                    if (!dest.writeBytes(&byte, 1)) return false;
                    havePrev = false;
                    continue;
                }
                if (!dest.writeBytes(&prev, 1)) return false;
            }
            prev = byte;
            havePrev = true;
        } else {
            if (!dest.writeBytes(&byte, 1)) return false;
        }
    }

    if (textMode && havePrev) {
        if (!dest.writeBytes(&prev, 1)) return false;
    }

    return true;
}

// ---------------------------------------------------------------------
// Reverse mode (-extract): reads the header written by writeHeader()
// and recovers the original source file contents.
// ---------------------------------------------------------------------
struct DecodedHeader {
    bool hasName = false;
    std::string name;

    bool hasAttrib = false;
    uint32_t attrib = 0;

    bool hasGuid = false;
    GUID guid{};
};

// Formats a GUID in the standard text representation (inverse of parseGuid()).
std::string formatGuid(const GUID& g) {
    char buf[40];
    std::snprintf(buf, sizeof(buf),
                  "%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X",
                  static_cast<unsigned long>(g.Data1), g.Data2, g.Data3,
                  g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3],
                  g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
    return std::string(buf);
}

// Reads and verifies the 4-byte header (magic + flags) and the optional
// name/attrib/guid sections, in exactly the same order that writeHeader()
// writes them. On return, the 'src' file cursor points to the start of the
// actual source content.
bool readHeader(Win32File& src, DecodedHeader& hdr) {
    uint32_t magicAndFlags = 0;
    if (!src.readBytes(&magicAndFlags, sizeof(magicAndFlags))) {
        std::fprintf(stderr, "Error: cannot read header (file too short?)\n");
        return false;
    }

    if ((magicAndFlags & 0x00FFFFFFu) != kMagicBase) {
        std::fprintf(stderr,
                     "Error: missing valid AddFileInfo signature "
                     "(expected lower 24 bits 0x%06X, found 0x%06X)\n",
                     kMagicBase, magicAndFlags & 0x00FFFFFFu);
        return false;
    }

    const uint32_t flagByte = (magicAndFlags >> 24) & 0xFFu;
    hdr.hasName   = (flagByte & 0x08) != 0;
    hdr.hasAttrib = (flagByte & 0x10) != 0;
    hdr.hasGuid   = (flagByte & 0x20) != 0;

    if (hdr.hasName) {
        uint8_t len = 0;
        if (!src.readBytes(&len, sizeof(len))) {
            std::fprintf(stderr, "Error: cannot read name length\n");
            return false;
        }
        hdr.name.resize(len);
        if (!src.readBytes(hdr.name.empty() ? nullptr : &hdr.name[0], len)) {
            std::fprintf(stderr, "Error: cannot read name\n");
            return false;
        }
    }

    if (hdr.hasAttrib) {
        if (!src.readBytes(&hdr.attrib, sizeof(hdr.attrib))) {
            std::fprintf(stderr, "Error: cannot read attrib\n");
            return false;
        }
    }

    if (hdr.hasGuid) {
        if (!src.readBytes(&hdr.guid, sizeof(GUID))) {
            std::fprintf(stderr, "Error: cannot read GUID\n");
            return false;
        }
    }

    return true;
}

// Copies the rest of the file (after the header) to the output file.
// If expandToCrLf==true, a CR is inserted before every LF, reconstructing
// Windows-style line endings. NOTE: CRLF->LF conversion on write (-text)
// is inherently lossy - if the original had single LFs mixed with CRLFs,
// the two can no longer be distinguished. This option therefore gives the
// best approximation ("always CRLF"), not a guaranteed 1:1 reconstruction.
bool exportBody(Win32File& src, Win32File& dest, bool expandToCrLf) {
    uint8_t byte = 0;
    while (src.readByte(byte)) {
        if (expandToCrLf && byte == '\n') {
            const uint8_t cr = '\r';
            if (!dest.writeBytes(&cr, 1)) return false;
        }
        if (!dest.writeBytes(&byte, 1)) return false;
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    Options opts;
    if (!parseArgs(argc, argv, opts)) {
        return -1;
    }

    if (opts.mode == Options::Mode::Decode) {
        Win32File src;
        if (!src.openForRead(opts.sourcePath)) {
            std::fprintf(stderr, "Error: cannot open source file: %s\n",
                         opts.sourcePath.c_str());
            return -1;
        }

        DecodedHeader hdr;
        if (!readHeader(src, hdr)) {
            return -1;
        }

        std::printf("AddFileInfo header:\n");
        std::printf("  name  : %s\n", hdr.hasName ? hdr.name.c_str() : "(none)");
        if (hdr.hasAttrib) {
            std::printf("  attrib: 0x%08X\n", hdr.attrib);
        } else {
            std::printf("  attrib: (none)\n");
        }
        std::printf("  guid  : %s\n", hdr.hasGuid ? formatGuid(hdr.guid).c_str() : "(none)");

        Win32File dest;
        if (!dest.openForWrite(opts.destPath)) {
            std::fprintf(stderr, "Error: cannot create destination file: %s\n",
                         opts.destPath.c_str());
            return -1;
        }

        if (!exportBody(src, dest, opts.expandCrLf)) {
            std::fprintf(stderr, "Error: failed to copy file contents\n");
            return -1;
        }

        src.close();
        dest.close();

        std::printf("Source content successfully exported to: %s\n",
                    opts.destPath.c_str());
        return 0;
    }

    if (opts.guidMode == Options::GuidMode::Create) {
        HRESULT hr = CoCreateGuid(&opts.guid);
        if (FAILED(hr)) {
            std::fprintf(stderr, "Error: failed to generate GUID (HRESULT 0x%08lX)\n",
                         static_cast<unsigned long>(hr));
            return -1;
        }
    }

    Win32File src;
    if (!src.openForRead(opts.sourcePath)) {
        std::fprintf(stderr, "Error: cannot open source file: %s\n",
                     opts.sourcePath.c_str());
        return -1;
    }

    Win32File dest;
    if (!dest.openForWrite(opts.destPath)) {
        std::fprintf(stderr, "Error: cannot create destination file: %s\n",
                     opts.destPath.c_str());
        return -1;
    }

    if (!writeHeader(dest, opts)) {
        std::fprintf(stderr, "Error: failed to write header to destination file\n");
        return -1;
    }

    if (!copyBody(src, dest, opts.textMode)) {
        std::fprintf(stderr, "Error: failed to copy file contents\n");
        return -1;
    }

    src.close();
    dest.close();

    std::printf("File successfully saved.\n");
    return 0;
}

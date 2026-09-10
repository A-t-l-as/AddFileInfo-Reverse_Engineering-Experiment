# PL - AddFileInfo - Reverse Engineering Experiment

Projekt odtwarzający działanie narzędzia **AddFileInfo** - małego programu Win32,
którego oryginalny plik `.exe` został przeanalizowany za pomocą Ghidry
(dekompilacja do C, ok. 9500 linii wygenerowanego kodu). Na tej podstawie
powstały dwie niezależne, działające implementacje:

- **`main.cpp`** - reimplementacja w idiomatycznym, nowoczesnym C++, korzystająca
  z Win32 API (`CreateFileA`, `CoCreateGuid`), możliwie wiernie odtwarzająca
  zachowanie oryginału.
- **`AddFileInfo2026_RE.py`** - port 1:1 logiki z `main.cpp` do Pythona, bez żadnej
  zależności od Win32 (zamiast `CoCreateGuid` używa modułu `uuid`). Działa na
  dowolnym systemie operacyjnym.

## Czym jest AddFileInfo

Program dopisuje na początku pliku binarny nagłówek z metadanymi (nazwa,
atrybut, GUID), a następnie kopiuje za nim zawartość pliku źródłowego -
opcjonalnie konwertując zakończenia linii CRLF → LF. Obsługuje też tryb
odwrotny (`-extract`), który z tak przygotowanego pliku odzyskuje oryginalną
zawartość oraz wypisuje zapisane w nagłówku metadane.

> **Uwaga:** to jest eksperyment z inżynierii wstecznej, a nie mechaniczne
> tłumaczenie każdej instrukcji z dekompilacji. Odtworzono rozpoznane,
> funkcjonalne zachowanie programu (parsowanie argumentów, generowanie/parsowanie
> GUID-a, zapis nagłówka, kopiowanie treści pliku), pomijając kod statycznie
> linkowanego runtime'u MSVC (obsługa wyjątków SEH, własny alokator, bufor
> strumienia plików itd.), który nie wpływa na wynikowy format pliku.

## Format nagłówka

```
uint32_t magicAndFlags;
    - dolne 24 bity (0x00FFFFFF) = stała magiczna 0x00D0A1FF
    - bit 27 (0x08000000) ustawiony <=> obecna sekcja "name"
    - bit 28 (0x10000000) ustawiony <=> obecna sekcja "attrib"
    - bit 29 (0x20000000) ustawiony <=> obecna sekcja "guid"

[ jeśli ustawiony bit name ]
uint8_t  nameLen;
char     name[nameLen];      // bez terminatora NUL w pliku

[ jeśli ustawiony bit attrib ]
uint32_t attrib;

[ jeśli ustawiony bit guid ]
uint8_t  guid[16];           // surowe 16 bajtów GUID-a, ułożone dokładnie
                              // tak jak struktura Win32 GUID w pamięci
                              // (Data1/Data2/Data3 little-endian, Data4
                              // jako 8 surowych bajtów)

[ dalej ]
<reszta zawartości pliku źródłowego, skopiowana 1:1 albo z konwersją
 CRLF -> LF, w zależności od trybu>
```

## Wymagania

- **`main.cpp`** - Windows + kompilator zgodny z MSVC (używa `<windows.h>`,
  `<objbase.h>` i linkuje `ole32.lib`).
- **`AddFileInfo2026_RE.py`** - Python 3.7+ (bez zewnętrznych zależności, tylko
  biblioteka standardowa).

## Użycie

Obie implementacje mają identyczny interfejs wiersza poleceń.

### Tryb zapisu (dodanie nagłówka do pliku)

```
AddFileInfo2026_RE.exe <plik_źródłowy> <plik_docelowy> [opcje]
python AddFileInfo2026_RE.py <plik_źródłowy> <plik_docelowy> [opcje]
AddFileInfo2026_RE.exe <plik wejściowy.def> <plik wyjściowy.int> -text -name <nazwa interfejsu>
```

Opcje:

| Opcja | Opis |
|---|---|
| `-text` | tryb tekstowy odczytu źródła (konwersja CRLF → LF) |
| `-binary` | tryb binarny odczytu źródła (domyślny) |
| `-name <nazwa>` | dopisuje sekcję z nazwą (max. 255 znaków) |
| `-guid <GUID>` | użyj podanego GUID-a, format `XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX` |
| `-guidCreate` | wygeneruj nowy, losowy GUID |
| `-guidNone` | nie dołączaj sekcji GUID (zachowanie domyślne) |
| `-attrib <wartość>` | dopisuje 32-bitową wartość atrybutu (dziesiętnie lub z prefiksem `0x`) |

Przykład:

```bash
python3 AddFileInfo2026_RE.py dane.txt dane.afi -name "dane.txt" -guidCreate -attrib 0x20
```

### Tryb odwrotny (`-extract`)

Odczytuje plik przetworzony wcześniej przez `AddFileInfo`/`add_file_info.py`,
wypisuje na standardowe wyjście zapisane metadane (nazwa/atrybut/GUID) i
zapisuje oryginalną zawartość do pliku wynikowego.

```
AddFileInfo2026_RE.py -extract <plik_binarny> <plik_wyjściowy> [-crlf]
```

- `-crlf` - wstawia `CR` przed każdym `LF`, odtwarzając zakończenia linii w
  stylu Windows. Konwersja CRLF → LF wykonana przy zapisie (`-text`) jest z
  natury stratna (nie da się odróżnić pojedynczych `LF` od tych, które
  wcześniej były `CRLF`), więc ta opcja daje jedynie najlepsze przybliżenie
  ("zawsze CRLF"), a nie gwarantowaną rekonstrukcję 1:1.

Przykład:

```bash
python3 AddFileInfo2026_RE.py -extract dane.afi dane_odzyskane.txt -crlf
```

## Status projektu

Projekt ma charakter eksperymentalny i edukacyjny - powstał jako ćwiczenie z
inżynierii wstecznej z wykorzystaniem sztucznej inteligencji. Proces powstawania
wyglądał następująco:

1. Oryginalny plik `.exe` został zdekompilowany w **Ghidrze**, uzyskując ok.
   9500 linii kodu w C.
2. Cały zrzut kodu z dekompilacji został przekazany modelowi AI z prośbą o
   przepisanie go na idiomatyczny, nowoczesny C++ (`main.cpp`) - z pominięciem
   fragmentów należących do statycznie linkowanego runtime'u MSVC (obsługa
   wyjątków SEH, własny alokator, bufor strumienia plików), a zachowaniem
   rozpoznanej logiki funkcjonalnej programu.
3. Model AI został poproszony o dopisanie do programu funkcjonalności
   odwrotnej (`-extract`), pozwalającej odczytać plik przetworzony wcześniej
   przez program i odzyskać z niego oryginalną zawartość oraz zapisane w
   nagłówku metadane - tej funkcji nie było w oryginalnym `.exe`.
4. Na końcu cały program został przepisany przez AI na Pythona
   (`AddFileInfo2026_RE.py`), zachowując 1:1 logikę z `main.cpp`, ale bez zależności
   od Win32 API, dzięki czemu działa na dowolnym systemie operacyjnym.

Format pliku i zachowanie narzędzia zostały zrekonstruowane na podstawie
analizy kodu z dekompilacji, a nie oficjalnej dokumentacji, więc mogą istnieć
przypadki brzegowe nieobsłużone identycznie jak w oryginale. Traktuj
projekt jako eksperyment, nie jako gotowe, przetestowane narzędzie.

---

# US - AddFileInfo - Reverse Engineering Experiment

A project recreating the behavior of **AddFileInfo** - a small Win32 tool
whose original `.exe` file was analyzed using Ghidra (decompiled to C,
roughly 9500 lines of generated code). Based on that analysis, two
independent, working implementations were produced:

- **`main.cpp`** - a reimplementation in idiomatic, modern C++, using the
  Win32 API (`CreateFileA`, `CoCreateGuid`), aiming to reproduce the
  original's behavior as faithfully as possible.
- **`AddFileInfo2026_RE.py`** - a 1:1 port of the logic from `main.cpp` to Python,
  with no dependency on Win32 (it uses the `uuid` module instead of
  `CoCreateGuid`). Runs on any operating system.

## What AddFileInfo does

The program prepends a binary header containing metadata (name, attribute,
GUID) to the beginning of a file, then copies the contents of the source
file after it - optionally converting CRLF → LF line endings. It also
supports a reverse mode (`-extract`), which recovers the original content
and prints the metadata stored in the header from a file prepared this way.

> **Note:** this is a reverse-engineering experiment, not a mechanical
> translation of every instruction from the decompilation. The recognized,
> functional behavior of the program was reconstructed (argument parsing,
> GUID generation/parsing, header writing, file content copying), while
> code belonging to the statically linked MSVC runtime (SEH exception
> handling, its own allocator, file stream buffer, etc.), which has no
> effect on the resulting file format, was omitted.

## Header format

```
uint32_t magicAndFlags;
    - low 24 bits (0x00FFFFFF) = magic constant 0x00D0A1FF
    - bit 27 (0x08000000) set <=> "name" section present
    - bit 28 (0x10000000) set <=> "attrib" section present
    - bit 29 (0x20000000) set <=> "guid" section present

[ if the name bit is set ]
uint8_t  nameLen;
char     name[nameLen];      // no NUL terminator in the file

[ if the attrib bit is set ]
uint32_t attrib;

[ if the guid bit is set ]
uint8_t  guid[16];           // raw 16 bytes of the GUID, laid out exactly
                              // as the Win32 GUID structure in memory
                              // (Data1/Data2/Data3 little-endian, Data4
                              // as 8 raw bytes)

[ followed by ]
<the rest of the source file's contents, copied 1:1 or with CRLF -> LF
 conversion, depending on the mode>
```

## Requirements

- **`main.cpp`** - Windows + an MSVC-compatible compiler (uses `<windows.h>`,
  `<objbase.h>` and links `ole32.lib`).
- **`AddFileInfo2026_RE.py`** - Python 3.7+ (no external dependencies, standard
  library only).

## Usage

Both implementations share an identical command-line interface.

### Write mode (adding a header to a file)

```
AddFileInfo2026_RE.exe <source_file> <target_file> [options]
python AddFileInfo2026_RE.py <source_file> <target_file> [options]
AddFileInfo2026_RE.exe <input file.def> <output file.int> -text -name <interface name>
```

Options:

| Option | Description |
|---|---|
| `-text` | text mode for reading the source (CRLF → LF conversion) |
| `-binary` | binary mode for reading the source (default) |
| `-name <name>` | appends a name section (max. 255 characters) |
| `-guid <GUID>` | use the given GUID, format `XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX` |
| `-guidCreate` | generate a new, random GUID |
| `-guidNone` | do not include a GUID section (default behavior) |
| `-attrib <value>` | appends a 32-bit attribute value (decimal or `0x`-prefixed) |

Example:

```bash
python3 AddFileInfo2026_RE.py data.txt data.afi -name "data.txt" -guidCreate -attrib 0x20
```

### Reverse mode (`-extract`)

Reads a file previously processed by `AddFileInfo`/`add_file_info.py`,
prints the stored metadata (name/attribute/GUID) to standard output, and
writes the original content to an output file.

```
AddFileInfo2026_RE.py -extract <binary_file> <output_file> [-crlf]
```

- `-crlf` - inserts a `CR` before every `LF`, restoring Windows-style line
  endings. Since the CRLF → LF conversion performed on write (`-text`) is
  inherently lossy (there's no way to distinguish plain `LF`s from ones that
  were originally `CRLF`), this option only provides a best approximation
  ("always CRLF"), not a guaranteed 1:1 reconstruction.

Example:

```bash
python3 AddFileInfo2026_RE.py -extract data.afi data_recovered.txt -crlf
```

## Project status

This project is experimental and educational in nature - it was created as
a reverse-engineering exercise using artificial intelligence. The process
went as follows:

1. The original `.exe` file was decompiled in **Ghidra**, yielding roughly
   9500 lines of C code.
2. The entire decompilation dump was handed to an AI model with a request
   to rewrite it into idiomatic, modern C++ (`main.cpp`) - omitting parts
   belonging to the statically linked MSVC runtime (SEH exception handling,
   its own allocator, file stream buffer), while preserving the program's
   recognized functional logic.
3. The AI model was asked to add reverse functionality (`-extract`) to the
   program, allowing a file previously processed by the program to be read
   back and its original content and header metadata recovered - this
   feature did not exist in the original `.exe`.
4. Finally, the entire program was rewritten by the AI into Python
   (`AddFileInfo2026_RE.py`), preserving the logic of `main.cpp` 1:1, but without
   any dependency on the Win32 API, so it runs on any operating system.

The file format and tool behavior were reconstructed based on analysis of
the decompiled code, not official documentation, so there may be edge cases
that aren't handled identically to the original. Treat this project as an
experiment, not a finished, tested tool.

#!/usr/bin/env python3
# ============================================================================
# add_file_info.py
#
# Rewrite of the "AddFileInfo" tool (originally C++/Win32) into Python.
# Logic is 1:1 with main.cpp, but without any dependency on the Win32 API
# (CreateFileA, CoCreateGuid) - it uses plain file operations and the
# `uuid` module to generate a random GUID. Works on any operating system.
#
# HEADER FORMAT (unchanged from the original):
#
#   uint32_t magicAndFlags;
#       - lower 24 bits (0x00FFFFFF) = magic constant 0x00D0A1FF
#       - bit 27 (0x08000000) set <=> "name" section present
#       - bit 28 (0x10000000) set <=> "attrib" section present
#       - bit 29 (0x20000000) set <=> "guid" section present
#
#   [ if the name bit is set ]
#   uint8_t  nameLen;
#   char     name[nameLen];       # no NUL terminator in the file
#
#   [ if the attrib bit is set ]
#   uint32_t attrib;
#
#   [ if the guid bit is set ]
#   uint8_t  guid[16];            # raw 16 bytes of the GUID (Data1..Data4,
#                                  # laid out in memory exactly as the
#                                  # Win32 GUID struct does: Data1/Data2/Data3
#                                  # little-endian, Data4 as 8 raw bytes)
#
#   [ then ]
#   <the rest of the source file's contents, copied 1:1 or with a
#    CRLF -> LF conversion, depending on the mode>
#
# COMMAND-LINE SYNTAX:
#
#   add_file_info.py <source_file> <dest_file> [options...]
#
#   options:
#     -text                  text mode when reading the source (CRLF -> LF)
#     -binary                binary mode when reading the source (default)
#     -name <name>           append a name section (max 255 characters)
#     -guid <GUID>            use the given GUID, format:
#                             XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX
#     -guidCreate            generate a new random GUID
#     -guidNone              do not include a GUID section (default behavior)
#     -attrib <value>        append a 32-bit attribute value
#                             (decimal or with a 0x prefix)
#
#   Reverse mode (read a file already processed by add_file_info.py):
#     add_file_info.py -extract <binary_file> <output_file> [-crlf]
# ============================================================================

from __future__ import annotations

import re
import struct
import sys
import uuid
from dataclasses import dataclass, field
from enum import Enum, auto
from typing import Optional

MAGIC_BASE = 0x00D0A1FF
NAME_FLAG_BIT = 0x08
ATTRIB_FLAG_BIT = 0x10
GUID_FLAG_BIT = 0x20


# ---------------------------------------------------------------------
# Simple representation of a GUID (equivalent to the Win32 GUID struct):
# Data1 (uint32), Data2 (uint16), Data3 (uint16), Data4 (8 bytes).
# ---------------------------------------------------------------------
@dataclass
class Guid:
    data1: int = 0
    data2: int = 0
    data3: int = 0
    data4: bytes = b"\x00" * 8

    def pack(self) -> bytes:
        # Data1/Data2/Data3 little-endian, Data4 as raw bytes (no reordering)
        # - exactly how the MSVC compiler lays this out in memory.
        return struct.pack("<IHH8s", self.data1, self.data2, self.data3, self.data4)

    @staticmethod
    def unpack(raw: bytes) -> "Guid":
        data1, data2, data3, data4 = struct.unpack("<IHH8s", raw)
        return Guid(data1, data2, data3, data4)

    @staticmethod
    def random() -> "Guid":
        b = uuid.uuid4().bytes  # standard big-endian layout of the UUID fields
        data1 = int.from_bytes(b[0:4], "big")
        data2 = int.from_bytes(b[4:6], "big")
        data3 = int.from_bytes(b[6:8], "big")
        data4 = b[8:16]
        return Guid(data1, data2, data3, data4)

    def __str__(self) -> str:
        d4 = "".join(f"{x:02X}" for x in self.data4)
        return (f"{self.data1:08X}-{self.data2:04X}-{self.data3:04X}-"
                f"{d4[0:4]}-{d4[4:16]}")


_GUID_RE = re.compile(
    r"^([0-9a-fA-F]{1,8})-([0-9a-fA-F]{1,4})-([0-9a-fA-F]{1,4})-"
    r"([0-9a-fA-F]{2})([0-9a-fA-F]{2})-"
    r"([0-9a-fA-F]{2})([0-9a-fA-F]{2})([0-9a-fA-F]{2})"
    r"([0-9a-fA-F]{2})([0-9a-fA-F]{2})([0-9a-fA-F]{2})$"
)


def parse_guid(text: str) -> Optional[Guid]:
    """Parses a GUID in the standard text format (without curly braces)."""
    m = _GUID_RE.match(text.strip())
    if not m:
        return None
    parts = [int(g, 16) for g in m.groups()]
    data1, data2, data3 = parts[0], parts[1], parts[2]
    data4 = bytes(parts[3:11])
    return Guid(data1, data2, data3, data4)


# ---------------------------------------------------------------------
# Command-line options
# ---------------------------------------------------------------------
class Mode(Enum):
    ENCODE = auto()
    DECODE = auto()


class GuidMode(Enum):
    NONE = auto()
    EXPLICIT = auto()
    CREATE = auto()


@dataclass
class Options:
    mode: Mode = Mode.ENCODE

    source_path: str = ""
    dest_path: str = ""

    text_mode: bool = False       # -text / -binary (binary by default)
    expand_crlf: bool = False     # only for Decode mode: -crlf

    has_name: bool = False
    name: str = ""

    has_attrib: bool = False
    attrib: int = 0

    guid_mode: GuidMode = GuidMode.NONE
    guid: Guid = field(default_factory=Guid)


def print_usage() -> None:
    print(
        "Usage: add_file_info.py <source_file> <dest_file> [options]\n\n"
        "Options:\n"
        "  -text                text mode for reading the source (CRLF -> LF)\n"
        "  -binary              binary mode for reading the source (default)\n"
        "  -name <name>         append a name section\n"
        "  -guid <GUID>          use the given GUID\n"
        "                       (format: XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX)\n"
        "  -guidCreate          generate a new random GUID\n"
        "  -guidNone            do not include a GUID section (default)\n"
        "  -attrib <value>      append an attribute value (decimal or 0x..)\n\n"
        "Reverse mode (read a file already processed by add_file_info.py):\n"
        "  add_file_info.py -extract <binary_file> <output_file> [-crlf]\n\n"
        "    Prints the header metadata (name/attrib/guid) to stdout\n"
        "    and writes the rest of the file (source content) to <output_file>.\n"
        "    -crlf   inserts a CR before every LF (restoring CRLF); without\n"
        "            this option the content is written exactly as it lies\n"
        "            in the container.\n",
        file=sys.stderr,
    )


def parse_args(argv: list[str]) -> Optional[Options]:
    """Returns Options, or None on a parse error (and prints a message)."""
    opts = Options()

    # Reverse mode: add_file_info.py -extract <binary_file> <output_file> [-crlf]
    if len(argv) >= 1 and argv[0].lower() == "-extract":
        opts.mode = Mode.DECODE

        if len(argv) < 3:
            print(
                "Error: syntax: add_file_info.py -extract <binary_file> "
                "<output_file> [-crlf]",
                file=sys.stderr,
            )
            return None

        opts.source_path = argv[1]
        opts.dest_path = argv[2]

        for arg in argv[3:]:
            if arg.lower() == "-crlf":
                opts.expand_crlf = True
            else:
                print(f"Error: unknown option in -extract mode: {arg}", file=sys.stderr)
                return None

        return opts

    if len(argv) < 2:
        print_usage()
        return None

    opts.source_path = argv[0]
    opts.dest_path = argv[1]

    i = 2
    while i < len(argv):
        arg = argv[i]
        low = arg.lower()

        if low == "-text":
            opts.text_mode = True
        elif low == "-binary":
            opts.text_mode = False
        elif low == "-guidcreate":
            opts.guid_mode = GuidMode.CREATE
        elif low == "-guidnone":
            opts.guid_mode = GuidMode.NONE
        elif low == "-name":
            if i + 1 >= len(argv):
                print("Error: missing value for -name", file=sys.stderr)
                return None
            i += 1
            opts.name = argv[i]
            if len(opts.name) > 255:
                print("Error: name is too long (max 255 characters)", file=sys.stderr)
                return None
            opts.has_name = len(opts.name) > 0
        elif low == "-guid":
            if i + 1 >= len(argv):
                print("Error: missing value for -guid", file=sys.stderr)
                return None
            i += 1
            g = parse_guid(argv[i])
            if g is None:
                print(f"Error: invalid GUID format: {argv[i]}", file=sys.stderr)
                return None
            opts.guid = g
            opts.guid_mode = GuidMode.EXPLICIT
        elif low == "-attrib":
            if i + 1 >= len(argv):
                print("Error: missing value for -attrib", file=sys.stderr)
                return None
            i += 1
            try:
                v = int(argv[i], 0)  # base 0 -> auto-detect the 0x prefix
                if v < 0:
                    raise ValueError
            except ValueError:
                print(f"Error: invalid value for -attrib: {argv[i]}", file=sys.stderr)
                return None
            opts.attrib = v & 0xFFFFFFFF
            opts.has_attrib = opts.attrib != 0
        else:
            print(f"Error: unknown option: {arg}", file=sys.stderr)
            print_usage()
            return None

        i += 1

    return opts


# ---------------------------------------------------------------------
# Writing the header / copying the content (Encode mode)
# ---------------------------------------------------------------------
def write_header(dest, opts: Options) -> None:
    has_guid = opts.guid_mode != GuidMode.NONE

    flag_nibble = 0
    if opts.has_name:
        flag_nibble |= NAME_FLAG_BIT
    if opts.has_attrib:
        flag_nibble |= ATTRIB_FLAG_BIT
    if has_guid:
        flag_nibble |= GUID_FLAG_BIT

    # The flags sit in bits 27-29 (0x08000000/0x10000000/0x20000000),
    # so flag_nibble (0x08/0x10/0x20) is shifted by 24, not by 20.
    magic_and_flags = (flag_nibble << 24) | MAGIC_BASE
    dest.write(struct.pack("<I", magic_and_flags))

    if opts.has_name:
        name_bytes = opts.name.encode("latin-1", errors="replace")
        dest.write(struct.pack("<B", len(name_bytes)))
        dest.write(name_bytes)

    if opts.has_attrib:
        dest.write(struct.pack("<I", opts.attrib))

    if has_guid:
        dest.write(opts.guid.pack())


def copy_body(src, dest, text_mode: bool) -> None:
    """Copies the source file's content to the destination file, optionally
    converting CRLF -> LF in text mode (just like the original with -text)."""
    data = src.read()
    if text_mode:
        data = data.replace(b"\r\n", b"\n")
    dest.write(data)


# ---------------------------------------------------------------------
# Reverse mode (-extract): reading the header and recovering the
# original source file content.
# ---------------------------------------------------------------------
@dataclass
class DecodedHeader:
    has_name: bool = False
    name: str = ""

    has_attrib: bool = False
    attrib: int = 0

    has_guid: bool = False
    guid: Guid = field(default_factory=Guid)


def read_header(src) -> Optional[DecodedHeader]:
    hdr = DecodedHeader()

    raw = src.read(4)
    if len(raw) != 4:
        print("Error: cannot read the header (file too short?)", file=sys.stderr)
        return None
    (magic_and_flags,) = struct.unpack("<I", raw)

    if (magic_and_flags & 0x00FFFFFF) != MAGIC_BASE:
        print(
            f"Error: missing a valid AddFileInfo signature "
            f"(expected lower 24 bits 0x{MAGIC_BASE:06X}, "
            f"found 0x{magic_and_flags & 0x00FFFFFF:06X})",
            file=sys.stderr,
        )
        return None

    flag_byte = (magic_and_flags >> 24) & 0xFF
    hdr.has_name = bool(flag_byte & 0x08)
    hdr.has_attrib = bool(flag_byte & 0x10)
    hdr.has_guid = bool(flag_byte & 0x20)

    if hdr.has_name:
        len_raw = src.read(1)
        if len(len_raw) != 1:
            print("Error: cannot read the name length", file=sys.stderr)
            return None
        name_len = len_raw[0]
        name_raw = src.read(name_len)
        if len(name_raw) != name_len:
            print("Error: cannot read the name", file=sys.stderr)
            return None
        hdr.name = name_raw.decode("latin-1", errors="replace")

    if hdr.has_attrib:
        raw = src.read(4)
        if len(raw) != 4:
            print("Error: cannot read the attribute", file=sys.stderr)
            return None
        (hdr.attrib,) = struct.unpack("<I", raw)

    if hdr.has_guid:
        raw = src.read(16)
        if len(raw) != 16:
            print("Error: cannot read the GUID", file=sys.stderr)
            return None
        hdr.guid = Guid.unpack(raw)

    return hdr


def export_body(src, dest, expand_to_crlf: bool) -> None:
    """Copies the rest of the file (after the header) to the output file. If
    expand_to_crlf==True, inserts a CR before every LF, restoring Windows-style
    line endings. NOTE: the CRLF->LF conversion on write (-text) is inherently
    lossy - this option gives the best approximation ("always CRLF"),
    not a guaranteed 1:1 reconstruction."""
    data = src.read()
    if expand_to_crlf:
        data = data.replace(b"\n", b"\r\n")
    dest.write(data)


# ---------------------------------------------------------------------
# main
# ---------------------------------------------------------------------
def main(argv: list[str]) -> int:
    opts = parse_args(argv)
    if opts is None:
        return 1

    if opts.mode == Mode.DECODE:
        try:
            src = open(opts.source_path, "rb")
        except OSError:
            print(f"Error: cannot open the source file: {opts.source_path}", file=sys.stderr)
            return 1

        with src:
            hdr = read_header(src)
            if hdr is None:
                return 1

            print("AddFileInfo header:")
            print(f"  name  : {hdr.name if hdr.has_name else '(none)'}")
            if hdr.has_attrib:
                print(f"  attrib: 0x{hdr.attrib:08X}")
            else:
                print("  attrib: (none)")
            print(f"  guid  : {hdr.guid if hdr.has_guid else '(none)'}")

            try:
                dest = open(opts.dest_path, "wb")
            except OSError:
                print(f"Error: cannot create the destination file: {opts.dest_path}", file=sys.stderr)
                return 1

            with dest:
                export_body(src, dest, opts.expand_crlf)

        print(f"Source content successfully exported to: {opts.dest_path}")
        return 0

    if opts.guid_mode == GuidMode.CREATE:
        opts.guid = Guid.random()

    try:
        src = open(opts.source_path, "rb")
    except OSError:
        print(f"Error: cannot open the source file: {opts.source_path}", file=sys.stderr)
        return 1

    with src:
        try:
            dest = open(opts.dest_path, "wb")
        except OSError:
            print(f"Error: cannot create the destination file: {opts.dest_path}", file=sys.stderr)
            return 1

        with dest:
            write_header(dest, opts)
            copy_body(src, dest, opts.text_mode)

    print("File successfully saved.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

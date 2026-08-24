#!/usr/bin/python3
##
## license:BSD-3-Clause
## copyright-holders:Stephen Spiller
"""Regenerate (or verify) the externally derived CoCoVGA character data.

Four committed files hold values that were taken from the CoCoVGA FPGA design
rather than from MAME:

    src/devices/video/cocovga_charrom.h         production generator deltas
    tests/emu/video/cocovga_charrom_oracle.h    character generator substitutions
    tests/emu/video/cocovga_lowercase_oracle.h  FPGA-generated lowercase glyphs
    tests/emu/video/cocovga_artifact_vectors.h  artifact palette selections

They are reproduced with authorization of rights holder Stephen Spiller and
are contributed under the BSD-3-Clause license those files declare.  The FPGA
sources themselves are not publicly redistributed, so each generated file pins
the snapshot it came from by commit and per-file SHA-256, and this script
reproduces the files byte for byte from that snapshot.

The production header and the test oracles are emitted by the same run, so the
values the device ships and the values the tests expect can never drift apart.

Two modes are supported:

  * --check on its own needs no private input at all.  It verifies the
    committed files against each other and against the metadata they carry:
    every file is re-emitted from the data and hashes it contains and compared
    byte for byte, the production tables are compared with the test oracles,
    the pinned snapshot and character generator ROM identification are compared
    with each other and with the ROM declarations in src/devices/video/mc6847.cpp
    and src/devices/video/cocovga.cpp, and the artifact vectors are checked
    against the invariants the palette layout guarantees.
    Anyone can run this against a checkout.

  * supplying the authorized inputs additionally regenerates (or, with --check,
    re-derives and compares) the values themselves from the pinned sources.
    --hdl and --roms drive the character generator files; --oracle adds the
    Icarus Verilog artifact traces.

Usage:
    python3 generate_cocovga_fixtures.py --check

    python3 generate_cocovga_fixtures.py \\
        --hdl <CoCoVGA FPGA source snapshot> \\
        --roms <directory holding the MC6847/MC6847T1 character ROM images> \\
        [--oracle <Icarus Verilog oracle directory with golden/*.jsonl>] \\
        [--out <test fixture directory, defaults to this script's directory>] \\
        [--devices-out <device source directory>] \\
        [--check]

Reviewers who do not have the FPGA snapshot can still confirm the emulation
against the public documentation linked from each generated file.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import re
import sys

GLYPH_HEIGHT = 12
LOWERCASE_GLYPHS = 32
TEXT_GLYPHS = 64
ARTIFACT_PALETTE_ENTRIES = 32

# The CoCoVGA FPGA sources are not publicly distributed.  Values derived from
# them are reproduced with authorization of the rights holder, and the exact
# revision is pinned here so that the derivation can be repeated and audited.
HDL_SNAPSHOT = "62b780a9d8117c2c591e69191b7136bba02f053a"

CHARACTER_GENERATOR_DOCS = (
    "http://cocovga.com/documentation/specifications/",
    "http://cocovga.com/documentation/software-mode-control/",
)

ARTIFACT_DOCS = (
    "http://cocovga.com/documentation/artifact-emulation/",
    "http://cocovga.com/documentation/software-mode-control/",
)

ROM_NOTES = {
    "mc6847_charset.bin": "CRC 3b22d071, SHA1 5e9d68e55e73cae3d28adaff34fe115e00029009",
    "mc6847t1_charset_needredump.bin": "CRC 42e62f8d, SHA1 1f09a076732a1e4b132cf298f0d1747df817e1c6",
}

PRODUCTION_HEADER = "cocovga_charrom.h"
CHARROM_ORACLE = "cocovga_charrom_oracle.h"
LOWERCASE_ORACLE = "cocovga_lowercase_oracle.h"
ARTIFACT_VECTORS = "cocovga_artifact_vectors.h"

# The CoCoVGA board carries its own copies of both MC6847 character generator
# ROMs, so the images the deltas are layered over are declared twice: once by
# the Motorola parts and once by the board.  Both declarations are checked, so
# that a redump cannot land in one place and leave the other stale.
DEVICE_SOURCES = ("mc6847.cpp", "cocovga.cpp")

GENERATOR_REFERENCE = "tests/emu/video/generate_cocovga_fixtures.py"


class Error(Exception):
    """A condition the user can act on, reported without a traceback."""


class SourceRef:
    """One pinned input file, identified by role, name and SHA-256."""

    def __init__(self, role: str, name: str, digest: str):
        self.role = role
        self.name = name
        self.digest = digest

    def lines(self) -> list[str]:
        return [
            f"        {self.role:<12}{self.name}",
            f"        {'sha256':<12}{self.digest}",
        ]


def sha256(path: pathlib.Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def hdl_ref(directory: pathlib.Path, name: str) -> SourceRef:
    return SourceRef("file", name, sha256(directory / name))


def srcclean_line(text: str) -> str:
    """Expand non-indent tabs the way MAME's srcclean does, at four-column stops."""
    indent = len(text) - len(text.lstrip("\t"))
    column = indent * 4
    out = [text[:indent]]
    for character in text[indent:]:
        if character == "\t":
            width = 4 - (column % 4)
            out.append(" " * width)
            column += width
        else:
            out.append(character)
            column += 1
    return "".join(out)


def assemble(lines: list[str]) -> str:
    return "\n".join(srcclean_line(line) for line in lines) + "\n"


def provenance_block(refs: list[SourceRef], docs: tuple[str, ...]) -> list[str]:
    """Emit the shared authorization, provenance and documentation comment lines."""
    lines = [
        "    Reproduced with authorization of rights holder Stephen Spiller and",
        "    contributed under the BSD-3-Clause license of this file.  The FPGA",
        "    sources are not publicly redistributed, so the exact revision the values",
        "    came from is pinned here by hash:",
        "",
        f"        {'snapshot':<12}{HDL_SNAPSHOT}",
    ]
    for ref in refs:
        lines.extend(ref.lines())
    lines.extend([
        "",
        "    Public documentation of the same behaviour:",
        "",
    ])
    lines.extend(f"        {url}" for url in docs)
    lines.extend([
        "",
        f"    {GENERATOR_REFERENCE} regenerates this file byte",
        "    for byte from the pinned sources; --check verifies it in place.",
    ])
    return lines


def rom_derivation_lines(rom_refs: list[SourceRef]) -> list[str]:
    """Describe how the ROM-derived base of each bank is produced."""
    lines = []
    for ref in rom_refs:
        lines.append(f"      * expand {ref.name} ({ROM_NOTES[ref.name]},")
        lines.append(f"        sha256 {ref.digest}) into 8x12 glyph rows;")
    return lines


# ---------------------------------------------------------------------------
# Verilog character ROM decoding
# ---------------------------------------------------------------------------

CHARACTER_RE = re.compile(r"^\s*[0-9]+'h([0-9a-fA-F]+)\s*:\s*$")
ROW_RE = re.compile(r"^\s*[0-9]+'h([0-9a-fA-F]+)\s*:\s*byteval\s*=\s*8'h([0-9a-fA-F]+)\s*;")
IF_BANK_RE = re.compile(r"^\s*if\s*\(\s*bank\s*\)")
ELSE_RE = re.compile(r"^\s*else\b")


def decode_character_rom(path: pathlib.Path, characters: int) -> list[list[list[int]]]:
    """Decode a CoCoVGA `case`-statement character ROM into [bank][char][row]."""
    rom = [[[None] * GLYPH_HEIGHT for _ in range(characters)] for _ in range(2)]
    character = None
    bank = None

    for raw in path.read_text().splitlines():
        line = raw.split("//", 1)[0]
        if not line.strip():
            continue

        match = CHARACTER_RE.match(line)
        if match:
            character = int(match.group(1), 16)
            bank = None
            continue

        if IF_BANK_RE.match(line):
            bank = 1
            continue

        if ELSE_RE.match(line):
            bank = 0
            continue

        match = ROW_RE.match(line)
        if match and character is not None and character < characters:
            row = int(match.group(1), 16)
            value = int(match.group(2), 16)
            if row >= GLYPH_HEIGHT:
                continue
            banks = (0, 1) if bank is None else (bank,)
            for index in banks:
                rom[index][character][row] = value

    for bank_index, bank_rows in enumerate(rom):
        for char_index, rows in enumerate(bank_rows):
            if any(value is None for value in rows):
                raise Error(
                    f"{path.name}: bank {bank_index} character {char_index} is incomplete: {rows}")
    return rom


# ---------------------------------------------------------------------------
# MC6847 character generator ROM expansion (written from the ROM layout)
# ---------------------------------------------------------------------------

def expand_internal_rom(rom: bytes) -> list[list[int]]:
    """Expand the packed 5x7 MC6847 internal font into 8x12 glyph rows."""
    font = []
    for character in range(TEXT_GLYPHS):
        rows = [0] * GLYPH_HEIGHT
        bit_shift = 7 - (character % 8)
        for glyph_row in range(7):
            value = 0
            for column in range(5):
                index = 224 + (character // 8) + glyph_row * 8 - column * 56
                if (rom[index] >> bit_shift) & 1:
                    value |= 0x20 >> column
            rows[3 + glyph_row] = value
        font.append(rows)
    return font


def expand_t1_rom(rom: bytes) -> list[list[int]]:
    """The MC6847T1 font ROM already holds 8x12 glyph rows."""
    return [
        [rom[character * GLYPH_HEIGHT + row] for row in range(GLYPH_HEIGHT)]
        for character in range(TEXT_GLYPHS)
    ]


def cocovga_t1_cell(font: list[list[int]]) -> list[list[int]]:
    """CoCoVGA draws the T1 bank one row lower and one pixel further left.

    The glyph bytes are most significant bit first, so the left shift moves
    every glyph pixel one column towards the left edge of CoCoVGA's cell.
    """
    return [
        [0] + [(rows[row - 1] << 1) & 0xff for row in range(1, GLYPH_HEIGHT)]
        for rows in font
    ]


# ---------------------------------------------------------------------------
# Emitters
# ---------------------------------------------------------------------------

def format_rows(rows: list[int]) -> str:
    return ", ".join(f"0x{value:02x}" for value in rows)


def emit_override_table(lines: list[str], declaration: str, entries: list[tuple[int, list[int]]]):
    lines.append(declaration)
    lines.append("{")
    for index, (character, rows) in enumerate(entries):
        comma = "," if index + 1 < len(entries) else ""
        lines.append(f"\t{{ {character}, {{ {format_rows(rows)} }} }}{comma}")
    lines.append("};")


def emit_lowercase_table(lines: list[str], declaration: str, rom: list[list[list[int]]]):
    lines.append(declaration)
    lines.append("{")
    for bank in range(2):
        lines.append("\t{")
        for character, rows in enumerate(rom[bank]):
            comma = "," if character + 1 < len(rom[bank]) else ""
            lines.append(f"\t\t{{ {format_rows(rows)} }}{comma}")
        lines.append("\t}," if bank == 0 else "\t}")
    lines.append("};")


def emit_production_header(
        lowercase: list[list[list[int]]],
        overrides: dict[int, list[tuple[int, list[int]]]],
        char_ref: SourceRef,
        lowercase_ref: SourceRef,
        rom_refs: list[SourceRef]) -> str:
    lines = []
    add = lines.append
    add("// license:BSD-3-Clause")
    add("// copyright-holders:Stephen Spiller")
    add("/*")
    add(f"    {PRODUCTION_HEADER}")
    add("")
    add("    CoCoVGA character generator deltas over the MC6847 character ROMs.")
    add("")
    add("    CoCoVGA regenerates the VDG character set inside its FPGA.  Almost")
    add("    every glyph it draws is the MC6847 or MC6847T1 character generator ROM")
    add("    image expanded into CoCoVGA's own 8x12 cell, and the device expands")
    add("    both ROM images it carries at run time, through the shared")
    add("    mc6847_charset helpers.  This header carries only what the FPGA")
    add("    generates itself and what therefore cannot be derived from those ROMs:")
    add("")
    add("      * the lowercase bank, which the FPGA synthesises and no MC6847 part")
    add("        stores; and")
    add("      * the small set of glyphs whose FPGA rows deliberately differ from the")
    add("        ROM expansion.")
    add("")
    add("    It is a set of CoCoVGA-specific generator deltas layered over the")
    add("    ROM-backed MC6847 bases, not a second copy of a character generator ROM:")
    add("    every glyph not named here comes from one of the two ROM images at run")
    add("    time, and the tables below are the differences the hardware introduces.")
    add("")
    lines.extend(provenance_block([char_ref, lowercase_ref], CHARACTER_GENERATOR_DOCS))
    add("")
    add("    Derivation, which can be repeated from those sources alone:")
    add("")
    add("      * decode every \"N'hCC:\" / \"4'hRR: byteval = 8'hVV;\" assignment in")
    add(f"        {char_ref.name} and {lowercase_ref.name} into")
    add("        rows[bank][character][row], where bank 1 is the MC6847T1 bank and")
    add("        bank 0 the standard MC6847 bank;")
    lines.extend(rom_derivation_lines(rom_refs))
    add("      * shift the T1 rows one row down and one pixel towards the left edge")
    add("        of the cell, as CoCoVGA does when it draws the T1 bank;")
    add("      * keep the lowercase bank as decoded, and list the text characters")
    add("        whose FPGA rows differ from the ROM-derived rows.")
    add("*/")
    add("")
    add("#ifndef MAME_VIDEO_COCOVGA_CHARROM_H")
    add("#define MAME_VIDEO_COCOVGA_CHARROM_H")
    add("")
    add("#pragma once")
    add("")
    add("#include \"mc6847_charset.h\"")
    add("")
    add("#include <cstdint>")
    add("")
    add("namespace cocovga")
    add("{")
    add("")
    add("// One text glyph whose FPGA rows replace the ROM-derived rows.")
    add("struct fpga_character_override")
    add("{")
    add("\tstd::uint8_t character;")
    add("\tstd::uint8_t rows[mc6847_charset::GLYPH_HEIGHT];")
    add("};")
    add("")
    add("// The lowercase bank the FPGA generates: [font bank][character][row].")
    emit_lowercase_table(
        lines,
        "inline constexpr std::uint8_t FPGA_LOWERCASE_ROWS"
        f"[2][{LOWERCASE_GLYPHS}][mc6847_charset::GLYPH_HEIGHT] =",
        lowercase)
    add("")
    add("// Text glyphs the FPGA draws differently from the MC6847 ROM expansion.")
    emit_override_table(
        lines,
        "inline constexpr fpga_character_override FPGA_STANDARD_OVERRIDES[] =",
        overrides[0])
    add("")
    add("// Text glyphs the FPGA draws differently from the MC6847T1 ROM expansion.")
    emit_override_table(
        lines,
        "inline constexpr fpga_character_override FPGA_T1_OVERRIDES[] =",
        overrides[1])
    add("")
    add("} // namespace cocovga")
    add("")
    add("#endif // MAME_VIDEO_COCOVGA_CHARROM_H")
    return assemble(lines)


def emit_charrom_oracle(
        overrides: dict[int, list[tuple[int, list[int]]]],
        char_ref: SourceRef,
        rom_refs: list[SourceRef]) -> str:
    lines = []
    add = lines.append
    add("// license:BSD-3-Clause")
    add("// copyright-holders:Stephen Spiller")
    add("/*")
    add(f"    {CHARROM_ORACLE}")
    add("")
    add("    Test-only expectations for the CoCoVGA character generator banks.")
    add("")
    add("    CoCoVGA renders the MC6847 and MC6847T1 character generator ROMs into")
    add("    its own 8x12 cell and substitutes its own bitmaps for a handful of")
    add("    characters.  The substituted bitmaps below were transcribed")
    add("    mechanically from the CoCoVGA FPGA sources.")
    add("")
    lines.extend(provenance_block([char_ref], CHARACTER_GENERATOR_DOCS))
    add("")
    add("    Derivation, which can be repeated from those sources alone:")
    add("")
    add("      * decode every \"N'hCC:\" / \"4'hRR: byteval = 8'hVV;\" assignment in")
    add(f"        {char_ref.name} into rom[bank][character][row], where bank 1 is the")
    add("        MC6847T1 bank and bank 0 the standard MC6847 bank;")
    lines.extend(rom_derivation_lines(rom_refs))
    add("      * shift the T1 rows one row down and one pixel towards the left edge")
    add("        of the cell, as CoCoVGA does when it draws the T1 bank into its own")
    add("        cell;")
    add("      * list the characters whose FPGA rows differ from the ROM-derived")
    add("        rows.  Every character that is not listed below matches the ROM")
    add("        expansion exactly, so the tests take those expectations from the")
    add("        character generator ROM fixture instead.")
    add("*/")
    add("")
    add("#ifndef MAME_TESTS_EMU_VIDEO_COCOVGA_CHARROM_ORACLE_H")
    add("#define MAME_TESTS_EMU_VIDEO_COCOVGA_CHARROM_ORACLE_H")
    add("")
    add("#pragma once")
    add("")
    add("#include \"mc6847_charset_fixture.h\"")
    add("")
    add("#include <cstdint>")
    add("")
    add("namespace cocovga::test")
    add("{")
    add("")
    add("struct fpga_character_override")
    add("{")
    add("\tstd::uint8_t character;")
    add("\tstd::uint8_t rows[12];")
    add("};")
    add("")
    emit_override_table(
        lines,
        "inline constexpr fpga_character_override FPGA_STANDARD_OVERRIDES[] =",
        overrides[0])
    add("")
    emit_override_table(
        lines,
        "inline constexpr fpga_character_override FPGA_T1_OVERRIDES[] =",
        overrides[1])
    add("")
    add("// Expected CoCoVGA text bank row, expressed against the character")
    add("// generator ROM fixture and the FPGA substitutions above.")
    add("inline std::uint8_t fpga_text_row(")
    add("\t\tbool t1_font,")
    add("\t\tstd::uint8_t character,")
    add("\t\tstd::uint8_t row)")
    add("{")
    add("\tcharacter &= 0x3f;")
    add("\tif (row >= 12)")
    add("\t\treturn 0;")
    add("")
    add("\tif (!t1_font)")
    add("\t{")
    add("\t\tfor (fpga_character_override const &entry : FPGA_STANDARD_OVERRIDES)")
    add("\t\t{")
    add("\t\t\tif (character == entry.character)")
    add("\t\t\t\treturn entry.rows[row];")
    add("\t\t}")
    add("\t\treturn mc6847_charset_test::internal_reference_row(")
    add("\t\t\t\tmc6847_charset_test::internal_rom(),")
    add("\t\t\t\tcharacter,")
    add("\t\t\t\trow);")
    add("\t}")
    add("")
    add("\tfor (fpga_character_override const &entry : FPGA_T1_OVERRIDES)")
    add("\t{")
    add("\t\tif (character == entry.character)")
    add("\t\t\treturn entry.rows[row];")
    add("\t}")
    add("")
    add("\t// the T1 bank sits one row lower and one pixel further left")
    add("\treturn row == 0")
    add("\t\t\t? 0")
    add("\t\t\t: std::uint8_t(")
    add("\t\t\t\t\tmc6847_charset_test::t1_reference_row(")
    add("\t\t\t\t\t\t\tmc6847_charset_test::t1_rom(),")
    add("\t\t\t\t\t\t\tcharacter,")
    add("\t\t\t\t\t\t\trow - 1) << 1);")
    add("}")
    add("")
    add("} // namespace cocovga::test")
    add("")
    add("#endif // MAME_TESTS_EMU_VIDEO_COCOVGA_CHARROM_ORACLE_H")
    return assemble(lines)


def emit_lowercase_oracle(rom: list[list[list[int]]], lowercase_ref: SourceRef) -> str:
    lines = []
    add = lines.append
    add("// license:BSD-3-Clause")
    add("// copyright-holders:Stephen Spiller")
    add("/*")
    add(f"    {LOWERCASE_ORACLE}")
    add("")
    add("    Test-only expectations for the CoCoVGA lowercase character bank.")
    add("")
    add("    The lowercase glyphs are generated by the FPGA itself rather than by")
    add("    any MC6847 character generator ROM, so the tests carry their own copy,")
    add("    transcribed mechanically from the CoCoVGA FPGA sources.")
    add("")
    lines.extend(provenance_block([lowercase_ref], CHARACTER_GENERATOR_DOCS))
    add("")
    add("    Derivation, which can be repeated from that source alone: decode every")
    add("    \"N'hCC:\" / \"4'hRR: byteval = 8'hVV;\" assignment into")
    add("    rows[bank][character][row], where bank 1 is the MC6847T1 bank and bank 0")
    add("    the standard MC6847 bank.")
    add("*/")
    add("")
    add("#ifndef MAME_TESTS_EMU_VIDEO_COCOVGA_LOWERCASE_ORACLE_H")
    add("#define MAME_TESTS_EMU_VIDEO_COCOVGA_LOWERCASE_ORACLE_H")
    add("")
    add("#pragma once")
    add("")
    add("#include <cstdint>")
    add("")
    add("namespace cocovga::test")
    add("{")
    add("")
    emit_lowercase_table(
        lines,
        f"inline constexpr std::uint8_t FPGA_LOWERCASE_ROWS[2][{LOWERCASE_GLYPHS}][12] =",
        rom)
    add("")
    add("} // namespace cocovga::test")
    add("")
    add("#endif // MAME_TESTS_EMU_VIDEO_COCOVGA_LOWERCASE_ORACLE_H")
    return assemble(lines)


def load_vectors(path: pathlib.Path) -> list[dict]:
    return [json.loads(line) for line in path.read_text().splitlines() if line.strip()]


def build_artifact_tables(oracle: pathlib.Path) -> dict[str, list]:
    """Collapse the Icarus Verilog traces into the four committed tables."""
    hdl_path = oracle / "golden" / "artifact-hdl-vectors.jsonl"
    simple_path = oracle / "golden" / "artifact-simple-vectors.jsonl"
    for path in (hdl_path, simple_path):
        if not path.is_file():
            raise Error(f"missing oracle trace: {path}")

    hdl_rows = load_vectors(hdl_path)
    simple_rows = load_vectors(simple_path)

    simple = {}
    mess = {}
    smart = {}
    for row in hdl_rows:
        key = (row["css"], row["swap"], row["pattern"], row["phase"])
        if row["mode"] == "simple_function":
            simple[key] = row["expected_palette_index"]
        elif row["mode"] == "mess":
            mess[key] = row["expected_palette_index"]
        elif row["mode"] == "smartifact":
            smart[key] = row["expected_palette_index"]
        else:
            raise Error(f"unexpected mode {row['mode']}")

    standard = {}
    fat_bits = {}
    for row in simple_rows:
        key = (row["css"], row["swap"], row["pattern"], row["source_bit_position"])
        target = standard if row["mode"] == "standard" else fat_bits
        target[key] = row["expected_palette_index"]

    # the simple selector ignores the output phase; collapse it after checking
    collapsed_simple = {}
    for (css, swap, pattern, phase), value in simple.items():
        key = (css, swap, pattern)
        if collapsed_simple.setdefault(key, value) != value:
            raise Error(f"simple selector depends on phase at {key}")

    # fat bits paints both halves of a pair with the same color
    for (css, swap, pattern, position), value in fat_bits.items():
        if fat_bits[(css, swap, pattern, 0)] != value:
            raise Error(f"fat bits selector depends on position at {css} {swap} {pattern}")

    return {
        "simple": [
            [[collapsed_simple[(css, swap, pattern)] for pattern in range(4)]
                for swap in range(2)]
            for css in range(2)],
        "standard": [
            [[[standard[(css, swap, pattern, position)] for position in range(2)]
                for pattern in range(4)]
                for swap in range(2)]
            for css in range(2)],
        "mess": [
            [[[mess[(css, swap, pattern, phase)] for phase in range(2)]
                for pattern in range(64)]
                for swap in range(2)]
            for css in range(2)],
        "smartifact": [
            [[[smart[(css, swap, pattern, phase)] for phase in range(4)]
                for pattern in range(64)]
                for swap in range(2)]
            for css in range(2)],
    }


def emit_artifact_vectors(tables: dict[str, list], refs: list[SourceRef]) -> str:
    lines = []
    add = lines.append
    add("// license:BSD-3-Clause")
    add("// copyright-holders:Stephen Spiller")
    add("/*")
    add(f"    {ARTIFACT_VECTORS}")
    add("")
    add("    Externally generated CoCoVGA artifact palette expectations.")
    add("")
    add("    Every value below was produced by simulating the CoCoVGA FPGA sources")
    add("    with Icarus Verilog and recording the palette index the hardware")
    add("    selects; none of it comes from the MAME implementation.")
    add("")
    lines.extend(provenance_block(refs, ARTIFACT_DOCS))
    add("")
    add("    Palette indices are absolute CoCoVGA artifact palette entries: entries")
    add("    0-15 are the CSS=1 half and entries 16-31 the CSS=0 half.")
    add("*/")
    add("")
    add("#ifndef MAME_TESTS_EMU_VIDEO_COCOVGA_ARTIFACT_VECTORS_H")
    add("#define MAME_TESTS_EMU_VIDEO_COCOVGA_ARTIFACT_VECTORS_H")
    add("")
    add("#pragma once")
    add("")
    add("#include <cstdint>")
    add("")
    add("namespace cocovga::test")
    add("{")
    add("")
    add("// [css][swap][pair pattern]")
    add("inline constexpr std::uint8_t SIMPLE_PALETTE_INDICES[2][2][4] =")
    add("{")
    for css in range(2):
        add("\t{")
        for swap in range(2):
            values = ", ".join(f"{tables['simple'][css][swap][pattern]:2d}" for pattern in range(4))
            comma = "," if swap == 0 else ""
            add(f"\t\t{{ {values} }}{comma}")
        add("\t}," if css == 0 else "\t}")
    add("};")
    add("")
    add("// [css][swap][pair pattern][pixel of the pair]")
    add("inline constexpr std::uint8_t STANDARD_PALETTE_INDICES[2][2][4][2] =")
    add("{")
    for css in range(2):
        add("\t{")
        for swap in range(2):
            add("\t\t{")
            for pattern in range(4):
                values = ", ".join(
                    f"{tables['standard'][css][swap][pattern][position]:2d}"
                    for position in range(2))
                comma = "," if pattern < 3 else ""
                add(f"\t\t\t{{ {values} }}{comma}")
            add("\t\t}," if swap == 0 else "\t\t}")
        add("\t}," if css == 0 else "\t}")
    add("};")
    add("")
    add("// [css][swap][six-tap window][pixel of the pair]")
    add("inline constexpr std::uint8_t MESS_PALETTE_INDICES[2][2][64][2] =")
    add("{")
    for css in range(2):
        add("\t{")
        for swap in range(2):
            add("\t\t{")
            for pattern in range(64):
                values = ", ".join(
                    f"{tables['mess'][css][swap][pattern][phase]:2d}" for phase in range(2))
                comma = "," if pattern < 63 else ""
                add(f"\t\t\t{{ {values} }}{comma}\t// {pattern:06b}")
            add("\t\t}," if swap == 0 else "\t\t}")
        add("\t}," if css == 0 else "\t}")
    add("};")
    add("")
    add("// [css][swap][six-tap window][output pixel of the doubled pair]")
    add("inline constexpr std::uint8_t SMARTIFACT_PALETTE_INDICES[2][2][64][4] =")
    add("{")
    for css in range(2):
        add("\t{")
        for swap in range(2):
            add("\t\t{")
            for pattern in range(64):
                values = ", ".join(
                    f"{tables['smartifact'][css][swap][pattern][phase]:2d}" for phase in range(4))
                comma = "," if pattern < 63 else ""
                add(f"\t\t\t{{ {values} }}{comma}\t// {pattern:06b}")
            add("\t\t}," if swap == 0 else "\t\t}")
        add("\t}," if css == 0 else "\t}")
    add("};")
    add("")
    add("} // namespace cocovga::test")
    add("")
    add("#endif // MAME_TESTS_EMU_VIDEO_COCOVGA_ARTIFACT_VECTORS_H")
    return assemble(lines)


# ---------------------------------------------------------------------------
# Committed file parsing, for checks that need no private input
# ---------------------------------------------------------------------------

BRACED_RE = re.compile(r"\{([^{}]*)\}")
SNAPSHOT_RE = re.compile(r"^\s+snapshot\s+([0-9a-f]{40})\s*$", re.MULTILINE)
PINNED_RE = re.compile(
    r"^\s+(file|trace)\s+(\S+)\n\s+sha256\s+([0-9a-fA-F]{64})\s*$", re.MULTILINE)
ROM_NOTE_RE = re.compile(
    r"^\s+\* expand (\S+) \(CRC ([0-9a-f]{8}), SHA1 ([0-9a-f]{40}),\n"
    r"\s+sha256 ([0-9a-f]{64})\) into 8x12 glyph rows;\s*$",
    re.MULTILINE)
ROM_LOAD_RE = re.compile(
    r"ROM_LOAD\w*\(\s*\"([^\"]+)\"[^)]*CRC\(([0-9a-f]{8})\)\s*SHA1\(([0-9a-f]{40})\)")
AUTHORIZATION = "Reproduced with authorization of rights holder Stephen Spiller"


def read_text(path: pathlib.Path) -> str:
    if not path.is_file():
        raise Error(f"missing generated file: {path}")
    return path.read_text(newline="")


def table_block(text: str, declaration: str, where: str) -> str:
    start = text.find(declaration)
    if start < 0:
        raise Error(f"{where}: no declaration matching '{declaration}'")
    end = text.find("\n};", start)
    if end < 0:
        raise Error(f"{where}: unterminated table '{declaration}'")
    return text[start:end]


def parse_byte_rows(text: str, declaration: str, where: str, banks: int, characters: int):
    block = table_block(text, declaration, where)
    rows = [
        [int(value, 16) for value in group.split(",") if value.strip()]
        for group in BRACED_RE.findall(block)
    ]
    if len(rows) != banks * characters:
        raise Error(
            f"{where}: '{declaration}' holds {len(rows)} rows, expected {banks * characters}")
    for row in rows:
        if len(row) != GLYPH_HEIGHT:
            raise Error(f"{where}: '{declaration}' has a row of {len(row)} bytes")
    return [rows[bank * characters:(bank + 1) * characters] for bank in range(banks)]


def parse_overrides(text: str, declaration: str, where: str) -> list[tuple[int, list[int]]]:
    block = table_block(text, declaration, where)
    characters = [int(value) for value in re.findall(r"\{\s*(\d+),\s*\{", block)]
    rows = [
        [int(value, 16) for value in group.split(",") if value.strip()]
        for group in BRACED_RE.findall(block)
    ]
    if len(characters) != len(rows):
        raise Error(f"{where}: '{declaration}' entry count does not match its row count")
    for row in rows:
        if len(row) != GLYPH_HEIGHT:
            raise Error(f"{where}: '{declaration}' has a row of {len(row)} bytes")
    return list(zip(characters, rows))


def parse_indices(text: str, declaration: str, where: str, shape: tuple[int, ...]) -> list:
    block = table_block(text, declaration, where)
    stripped = "\n".join(line.split("//", 1)[0] for line in block.splitlines())
    values = [int(value) for value in re.findall(r"-?\d+", stripped.split("=", 1)[1])]
    expected = 1
    for extent in shape:
        expected *= extent
    if len(values) != expected:
        raise Error(
            f"{where}: '{declaration}' holds {len(values)} values, expected {expected}")

    def fold(flat: list[int], dimensions: tuple[int, ...]):
        if len(dimensions) == 1:
            return flat
        stride = len(flat) // dimensions[0]
        return [fold(flat[i * stride:(i + 1) * stride], dimensions[1:])
                for i in range(dimensions[0])]

    return fold(values, shape)


def parse_refs(text: str, where: str) -> list[SourceRef]:
    refs = [SourceRef(role, name, digest.lower()) for role, name, digest in PINNED_RE.findall(text)]
    if not refs:
        raise Error(f"{where}: no pinned 'file'/'trace' + 'sha256' provenance entries")
    return refs


def parse_snapshot(text: str, where: str) -> str:
    match = SNAPSHOT_RE.search(text)
    if not match:
        raise Error(f"{where}: no pinned snapshot commit")
    return match.group(1)


def parse_rom_refs(text: str, where: str) -> list[SourceRef]:
    refs = []
    for name, crc, sha1, digest in ROM_NOTE_RE.findall(text):
        expected = ROM_NOTES.get(name)
        if expected is None:
            raise Error(f"{where}: unknown character generator ROM '{name}'")
        if expected != f"CRC {crc}, SHA1 {sha1}":
            raise Error(f"{where}: {name} is pinned as 'CRC {crc}, SHA1 {sha1}', expected '{expected}'")
        refs.append(SourceRef("file", name, digest))
    if len(refs) != len(ROM_NOTES):
        raise Error(f"{where}: {len(refs)} character generator ROMs described, expected {len(ROM_NOTES)}")
    return refs


# ---------------------------------------------------------------------------
# Checks that need no private input
# ---------------------------------------------------------------------------

class Report:
    def __init__(self):
        self.failures = 0

    def ok(self, message: str):
        print(f"ok        {message}")

    def fail(self, message: str):
        print(f"FAILED    {message}")
        self.failures += 1

    def check(self, condition: bool, message: str):
        if condition:
            self.ok(message)
        else:
            self.fail(message)


def check_formatting(report: Report, name: str, text: str):
    if "\r" in text:
        report.fail(f"{name} contains carriage returns")
    elif not text.endswith("\n"):
        report.fail(f"{name} does not end with a newline")
    elif any(line != srcclean_line(line) for line in text.split("\n")):
        report.fail(f"{name} is not srcclean-stable")
    else:
        report.ok(f"{name} is srcclean-stable LF text")


def check_provenance(report: Report, name: str, text: str, docs: tuple[str, ...]):
    if AUTHORIZATION not in text:
        report.fail(f"{name} carries no rights holder authorization statement")
    else:
        report.ok(f"{name} carries the rights holder authorization statement")

    snapshot = parse_snapshot(text, name)
    report.check(snapshot == HDL_SNAPSHOT, f"{name} pins snapshot {HDL_SNAPSHOT}")

    missing = [url for url in docs if url not in text]
    report.check(not missing, f"{name} links the public documentation")

    report.check(
        GENERATOR_REFERENCE in text,
        f"{name} names the generator that reproduces it")


def check_artifact_invariants(report: Report, tables: dict[str, list]):
    values = []
    for table in tables.values():
        stack = [table]
        while stack:
            item = stack.pop()
            if isinstance(item, list):
                stack.extend(item)
            else:
                values.append(item)
    in_range = all(0 <= value < ARTIFACT_PALETTE_ENTRIES for value in values)
    report.check(in_range, "artifact palette indices are within the 32-entry palette")

    half = ARTIFACT_PALETTE_ENTRIES // 2
    css_split = True
    for name, table in tables.items():
        for css in range(2):
            stack = [table[css]]
            while stack:
                item = stack.pop()
                if isinstance(item, list):
                    stack.extend(item)
                elif (item < half) != (css == 1):
                    css_split = False
    report.check(css_split, "artifact palette indices stay in their CSS half")

    background = set()
    consistent = True
    for css in range(2):
        for swap in range(2):
            for pattern in range(4):
                for position in range(2):
                    value = tables["standard"][css][swap][pattern][position]
                    lit = (pattern >> (1 - position)) & 1
                    if lit:
                        if value != tables["simple"][css][swap][pattern]:
                            consistent = False
                    else:
                        background.add((css, value))
    report.check(consistent, "standard artifact pixels reuse the simple selector when lit")
    report.check(
        len(background) == 2,
        "unlit standard artifact pixels use one background entry per CSS half")


def offline_check(fixtures: pathlib.Path, devices: pathlib.Path) -> int:
    """Verify the committed files against each other without any private input."""
    report = Report()

    production_text = read_text(devices / PRODUCTION_HEADER)
    charrom_text = read_text(fixtures / CHARROM_ORACLE)
    lowercase_text = read_text(fixtures / LOWERCASE_ORACLE)
    artifact_text = read_text(fixtures / ARTIFACT_VECTORS)

    for name, text, docs in (
            (PRODUCTION_HEADER, production_text, CHARACTER_GENERATOR_DOCS),
            (CHARROM_ORACLE, charrom_text, CHARACTER_GENERATOR_DOCS),
            (LOWERCASE_ORACLE, lowercase_text, CHARACTER_GENERATOR_DOCS),
            (ARTIFACT_VECTORS, artifact_text, ARTIFACT_DOCS)):
        check_formatting(report, name, text)
        check_provenance(report, name, text, docs)

    production_lowercase = parse_byte_rows(
        production_text,
        "inline constexpr std::uint8_t FPGA_LOWERCASE_ROWS",
        PRODUCTION_HEADER,
        2,
        LOWERCASE_GLYPHS)
    oracle_lowercase = parse_byte_rows(
        lowercase_text,
        "inline constexpr std::uint8_t FPGA_LOWERCASE_ROWS",
        LOWERCASE_ORACLE,
        2,
        LOWERCASE_GLYPHS)
    report.check(
        production_lowercase == oracle_lowercase,
        f"{PRODUCTION_HEADER} and {LOWERCASE_ORACLE} hold the same lowercase bank")

    production_overrides = {
        0: parse_overrides(
            production_text,
            "inline constexpr fpga_character_override FPGA_STANDARD_OVERRIDES[]",
            PRODUCTION_HEADER),
        1: parse_overrides(
            production_text,
            "inline constexpr fpga_character_override FPGA_T1_OVERRIDES[]",
            PRODUCTION_HEADER),
    }
    oracle_overrides = {
        0: parse_overrides(
            charrom_text,
            "inline constexpr fpga_character_override FPGA_STANDARD_OVERRIDES[]",
            CHARROM_ORACLE),
        1: parse_overrides(
            charrom_text,
            "inline constexpr fpga_character_override FPGA_T1_OVERRIDES[]",
            CHARROM_ORACLE),
    }
    report.check(
        production_overrides == oracle_overrides,
        f"{PRODUCTION_HEADER} and {CHARROM_ORACLE} hold the same character substitutions")

    for bank, entries in production_overrides.items():
        characters = [character for character, _ in entries]
        report.check(
            characters == sorted(set(characters)) and all(c < TEXT_GLYPHS for c in characters),
            f"bank {bank} substitutions are sorted, unique and within the 64 text glyphs")

    production_refs = {ref.name: ref for ref in parse_refs(production_text, PRODUCTION_HEADER)}
    charrom_refs = {ref.name: ref for ref in parse_refs(charrom_text, CHARROM_ORACLE)}
    lowercase_refs = {ref.name: ref for ref in parse_refs(lowercase_text, LOWERCASE_ORACLE)}
    artifact_refs = parse_refs(artifact_text, ARTIFACT_VECTORS)

    for name, refs in (
            (CHARROM_ORACLE, charrom_refs),
            (LOWERCASE_ORACLE, lowercase_refs)):
        shared = set(refs) & set(production_refs)
        report.check(
            bool(shared) and all(refs[key].digest == production_refs[key].digest for key in shared),
            f"{name} pins the same FPGA sources as {PRODUCTION_HEADER}")

    production_roms = parse_rom_refs(production_text, PRODUCTION_HEADER)
    charrom_roms = parse_rom_refs(charrom_text, CHARROM_ORACLE)
    report.check(
        [(ref.name, ref.digest) for ref in production_roms]
        == [(ref.name, ref.digest) for ref in charrom_roms],
        "both character generator files identify the same MC6847 ROM images")

    for device_source in DEVICE_SOURCES:
        device_text = read_text(devices / device_source)
        declared = {name: f"CRC {crc}, SHA1 {sha1}" for name, crc, sha1 in ROM_LOAD_RE.findall(device_text)}
        report.check(
            all(declared.get(ref.name) == ROM_NOTES[ref.name] for ref in production_roms),
            f"the ROM images described here are the ones {device_source} declares")

    artifact_tables = {
        "simple": parse_indices(
            artifact_text,
            "inline constexpr std::uint8_t SIMPLE_PALETTE_INDICES[2][2][4]",
            ARTIFACT_VECTORS,
            (2, 2, 4)),
        "standard": parse_indices(
            artifact_text,
            "inline constexpr std::uint8_t STANDARD_PALETTE_INDICES[2][2][4][2]",
            ARTIFACT_VECTORS,
            (2, 2, 4, 2)),
        "mess": parse_indices(
            artifact_text,
            "inline constexpr std::uint8_t MESS_PALETTE_INDICES[2][2][64][2]",
            ARTIFACT_VECTORS,
            (2, 2, 64, 2)),
        "smartifact": parse_indices(
            artifact_text,
            "inline constexpr std::uint8_t SMARTIFACT_PALETTE_INDICES[2][2][64][4]",
            ARTIFACT_VECTORS,
            (2, 2, 64, 4)),
    }
    check_artifact_invariants(report, artifact_tables)

    # Re-emit every file from the data and metadata it carries.  This proves the
    # committed text is exactly what this generator produces for those values,
    # without needing the sources the values came from.
    char_ref = charrom_refs.get("CharRom.v")
    lowercase_ref = lowercase_refs.get("LowercaseCharRom.v")
    if char_ref is None or lowercase_ref is None:
        raise Error("the character generator files do not pin CharRom.v and LowercaseCharRom.v")

    rebuilt = {
        PRODUCTION_HEADER: emit_production_header(
            production_lowercase,
            production_overrides,
            production_refs["CharRom.v"],
            production_refs["LowercaseCharRom.v"],
            production_roms),
        CHARROM_ORACLE: emit_charrom_oracle(oracle_overrides, char_ref, charrom_roms),
        LOWERCASE_ORACLE: emit_lowercase_oracle(oracle_lowercase, lowercase_ref),
        ARTIFACT_VECTORS: emit_artifact_vectors(artifact_tables, artifact_refs),
    }
    for name, text in (
            (PRODUCTION_HEADER, production_text),
            (CHARROM_ORACLE, charrom_text),
            (LOWERCASE_ORACLE, lowercase_text),
            (ARTIFACT_VECTORS, artifact_text)):
        report.check(
            rebuilt[name] == text,
            f"{name} matches what this generator emits for the data it carries")

    print()
    if report.failures:
        print(f"{report.failures} consistency check(s) failed")
        return 1
    print("committed CoCoVGA character data is self-consistent")
    return 0


# ---------------------------------------------------------------------------
# Regeneration from the authorized inputs
# ---------------------------------------------------------------------------

def write_or_check(path: pathlib.Path, content: str, check: bool) -> bool:
    if check:
        current = path.read_text(newline="") if path.exists() else None
        if current == content:
            print(f"unchanged {path}")
            return True
        print(f"DIFFERS   {path}")
        return False
    path.write_text(content, newline="")
    print(f"wrote     {path}")
    return True


def require_directory(path: pathlib.Path, option: str) -> pathlib.Path:
    if not path.is_dir():
        raise Error(f"{option} is not a directory: {path}")
    return path


def require_file(path: pathlib.Path, option: str) -> pathlib.Path:
    if not path.is_file():
        raise Error(f"{option} does not hold {path.name}: {path}")
    return path


def regenerate_character_data(
        hdl: pathlib.Path,
        roms: pathlib.Path,
        fixtures: pathlib.Path,
        devices: pathlib.Path,
        check: bool) -> bool:
    char_rom_path = require_file(hdl / "CharRom.v", "--hdl")
    lowercase_path = require_file(hdl / "LowercaseCharRom.v", "--hdl")
    internal_rom_path = require_file(roms / "mc6847_charset.bin", "--roms")
    t1_rom_path = require_file(roms / "mc6847t1_charset_needredump.bin", "--roms")

    fpga = decode_character_rom(char_rom_path, TEXT_GLYPHS)
    lowercase = decode_character_rom(lowercase_path, LOWERCASE_GLYPHS)

    standard_font = expand_internal_rom(internal_rom_path.read_bytes())
    t1_font = cocovga_t1_cell(expand_t1_rom(t1_rom_path.read_bytes()))

    overrides = {0: [], 1: []}
    for character in range(TEXT_GLYPHS):
        if fpga[0][character] != standard_font[character]:
            overrides[0].append((character, fpga[0][character]))
        if fpga[1][character] != t1_font[character]:
            overrides[1].append((character, fpga[1][character]))

    print(f"standard bank overrides: {[entry[0] for entry in overrides[0]]}")
    print(f"T1 bank overrides:       {[entry[0] for entry in overrides[1]]}")

    char_ref = hdl_ref(hdl, "CharRom.v")
    lowercase_ref = hdl_ref(hdl, "LowercaseCharRom.v")
    rom_refs = [
        SourceRef("file", internal_rom_path.name, sha256(internal_rom_path)),
        SourceRef("file", t1_rom_path.name, sha256(t1_rom_path)),
    ]

    results = [
        write_or_check(
            devices / PRODUCTION_HEADER,
            emit_production_header(lowercase, overrides, char_ref, lowercase_ref, rom_refs),
            check),
        write_or_check(
            fixtures / CHARROM_ORACLE,
            emit_charrom_oracle(overrides, char_ref, rom_refs),
            check),
        write_or_check(
            fixtures / LOWERCASE_ORACLE,
            emit_lowercase_oracle(lowercase, lowercase_ref),
            check),
    ]
    return all(results)


def regenerate_artifact_vectors(
        hdl: pathlib.Path,
        oracle: pathlib.Path,
        fixtures: pathlib.Path,
        check: bool) -> bool:
    tables = build_artifact_tables(oracle)
    golden = oracle / "golden"
    refs = [
        hdl_ref(hdl, "pixelGen.v"),
        hdl_ref(hdl, "ctrlRegs.v"),
        SourceRef(
            "trace",
            "artifact-hdl-vectors.jsonl",
            sha256(golden / "artifact-hdl-vectors.jsonl")),
        SourceRef(
            "trace",
            "artifact-simple-vectors.jsonl",
            sha256(golden / "artifact-simple-vectors.jsonl")),
    ]
    return write_or_check(
        fixtures / ARTIFACT_VECTORS,
        emit_artifact_vectors(tables, refs),
        check)


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument(
        "--hdl",
        type=pathlib.Path,
        help="CoCoVGA FPGA source snapshot (authorized input, not redistributed)")
    parser.add_argument(
        "--roms",
        type=pathlib.Path,
        help="directory holding the MC6847 and MC6847T1 character generator ROM images")
    parser.add_argument(
        "--oracle",
        type=pathlib.Path,
        help="Icarus Verilog oracle directory holding golden/*.jsonl artifact traces")
    parser.add_argument(
        "--out",
        type=pathlib.Path,
        default=pathlib.Path(__file__).resolve().parent,
        help="test fixture directory (defaults to this script's directory)")
    parser.add_argument(
        "--devices-out",
        type=pathlib.Path,
        default=pathlib.Path(__file__).resolve().parents[3] / "src" / "devices" / "video",
        help="device source directory holding the production character data header")
    parser.add_argument(
        "--check",
        action="store_true",
        help="verify the committed files instead of writing them")
    args = parser.parse_args()

    fixtures = require_directory(args.out, "--out")
    devices = require_directory(args.devices_out, "--devices-out")

    if args.hdl is None and args.roms is None and args.oracle is None:
        if not args.check:
            raise Error(
                "regeneration needs the authorized inputs: pass --hdl and --roms "
                "(and --oracle for the artifact vectors), or run --check on its own "
                "to verify the committed files")
        return offline_check(fixtures, devices)

    if (args.hdl is None) != (args.roms is None):
        raise Error("--hdl and --roms are used together")
    if args.oracle is not None and args.hdl is None:
        raise Error("--oracle also needs --hdl, which pins the simulated sources")

    results = []
    if args.hdl is not None:
        results.append(regenerate_character_data(
            require_directory(args.hdl, "--hdl"),
            require_directory(args.roms, "--roms"),
            fixtures,
            devices,
            args.check))
    if args.oracle is not None:
        results.append(regenerate_artifact_vectors(
            require_directory(args.hdl, "--hdl"),
            require_directory(args.oracle, "--oracle"),
            fixtures,
            args.check))

    if not all(results):
        return 1
    if not args.check:
        return 0

    print()
    return offline_check(fixtures, devices)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Error as error:
        print(f"error: {error}", file=sys.stderr)
        sys.exit(2)

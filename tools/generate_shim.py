import argparse
import re
import struct
import sys
from pathlib import Path
from elftools.elf.elffile import ELFFile

PROJECT_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_FIRMWARE_DIR = PROJECT_ROOT / "firmware" / "2025-15270"
DEFAULT_ELF_FILE = DEFAULT_FIRMWARE_DIR / "armfir.elf"
DEFAULT_MAP_FILE = PROJECT_ROOT / "tools" / "interruptmap.txt"

# ARM Hex opcodes (Little Endian):
# push {r0}     -> stmdb sp!, {r0} -> 0xe92d0001 -> \x01\x00\x2d\xe9
# push {lr}     -> stmdb sp!, {lr} -> 0xe92d4000 -> \x00\x40\x2d\xe9
# swi 0x10xxxx  -> svc 0x010xxxx   -> 0xef01xxxx -> (xx xx)\x01\xef
# Regex captures the two bytes of the SWI number index
COMBINED_PATTERN = re.compile(
    # Group 1 & 2: push {r0}; push {lr}; swi 0x10xxx
    b'(\x01\x00\x2d\xe9\x00\x40\x2d\xe9(..)\x01\xef)|'
    # Group 3,4,5,6: mov Ra, #0x51000000; ldr Rb, [Ra, #64]
    b'(\x51(.)\xa0\xe3\x40(.)(.)\xe5)',
    re.DOTALL
)

def load_swi_map(filepath):
    swi_map = {}
    try:
        with open(filepath, 'r') as f:
            for line in f:
                line = line.strip().rstrip(',')
                if '=' in line:
                    name, val = line.split('=')
                    swi_map[int(val.strip(), 16)] = name.strip()
    except FileNotFoundError:
        print(f"Warning: {filepath} not found. Output will use SWI_0xHEX fallbacks.")
    return swi_map

def emit_words(f, chunk, start_addr, entry_addr=None):
    """Formats raw bytes into 4-word assembly lines and accurately inserts entry_point:"""
    i = 0
    while i < len(chunk):
        curr_addr = start_addr + i

        # Output label if we hit the entry address
        if entry_addr is not None and curr_addr == entry_addr:
            f.write("entry_point:\n")

        # Calculate how many bytes to process before stopping for an entry_point check
        step = 16
        if entry_addr is not None and curr_addr < entry_addr < curr_addr + 16:
            step = entry_addr - curr_addr

        line_bytes = chunk[i:i+step]

        # Safe padding
        while len(line_bytes) % 4 != 0:
            line_bytes += b'\x00'

        words = [struct.unpack('<I', line_bytes[j:j+4])[0] for j in range(0, len(line_bytes), 4)]
        words_str = ", ".join(f"0x{w:08x}" for w in words)
        f.write(f".word {words_str} @{curr_addr:08x}\n")

        i += step

def generate_asm(filepath, data, base_addr, section_name, is_text, swi_map, entry_addr=None):
    with open(filepath, 'w') as f:
        f.write(".arch armv5te\n")
        if is_text:
            f.write(".arm\n")
            f.write(f'.section {section_name},"ax"\n')
            f.write(".global entry_point\n")

            last_idx = 0
            for match in COMBINED_PATTERN.finditer(data):
                # Write standard words up to the match
                chunk = data[last_idx:match.start()]
                if chunk:
                    emit_words(f, chunk, base_addr + last_idx, entry_addr)

                # Check if entry point falls exactly on the replaced macro
                if entry_addr == base_addr + match.start():
                    f.write("entry_point:\n")

                if match.group(1):
                    # --- SWI Sequence Matched ---
                    swi_lower = struct.unpack('<H', match.group(2))[0]
                    swi_num = 0x10000 + swi_lower
                    symbol = swi_map.get(swi_num, f"SWI_{swi_num:05x}")
                    f.write(f"b {symbol}\nnop\nnop\n")

                elif match.group(3):
                    # --- Timer Sequence Matched ---
                    ra_mov = match.group(4)[0] >> 4
                    rb_ldr = match.group(5)[0] >> 4
                    ra_ldr = match.group(6)[0] & 0x0F

                    # Verify Ra matches in both instructions to avoid false positives
                    if ra_mov == ra_ldr:
                        f.write(f"bl gettimer4r{rb_ldr}\nnop\n")
                    else:
                        emit_words(f, match.group(3), base_addr + match.start(), entry_addr)

                last_idx = match.end()

            # Write remaining data
            chunk = data[last_idx:]
            if chunk:
                emit_words(f, chunk, base_addr + last_idx, entry_addr)

        else:
            f.write(f'.section {section_name},"wa"\n')
            emit_words(f, data, base_addr)

def generate_linker_script(filepath, ro_addr, rw_addr, zi_addr, zi_size):
    end_fixed = zi_addr + zi_size
    ld_content = f"""ENTRY(_start)

MEMORY {{
    RAM (rwx) : ORIGIN = 0x{ro_addr:08x}, LENGTH = 256M
}}

SECTIONS {{
    /* --- Fixed custom sections first --- */
    .ER_RO 0x{ro_addr:08x} : {{
        *(.ER_RO_text .ER_RO.*)
    }} > RAM

    .ER_RW 0x{rw_addr:08x} : {{
        *(.ER_RW_data .ER_RW.*)
    }} > RAM

    .empty 0x{zi_addr:08x} (NOLOAD) : {{
        . += 0x{zi_size:x};
    }} > RAM

    /* --- Set location counter to end of last fixed section --- */
    __end_fixed = 0x{zi_addr:08x} + 0x{zi_size:x};  /* 0x{end_fixed:08x} */
    . = __end_fixed;

    /* --- Standard sections AFTER fixed sections --- */
    .text : {{
        *(.text .text.*)
    }} > RAM

    .rodata : {{
        *(.rodata .rodata.*)
    }} > RAM

    .data : {{
        *(.data .data.*)
    }} > RAM

    .bss : {{
        *(.bss .bss.*)
        *(COMMON)
    }} > RAM

    /* --- Dynamic sections --- */
    .plt : {{ *(.plt) }} > RAM
    .got : {{ *(.got.plt) *(.got) }} > RAM
    .dynamic : {{ *(.dynamic) }} > RAM
    .dynsym : {{ *(.dynsym) }} > RAM
    .dynstr : {{ *(.dynstr) }} > RAM
    .rel.dyn : {{ *(.rel.dyn) }} > RAM

    /* --- Heap/stack boundary --- */
    . = ALIGN(8);
    _end = .;
    PROVIDE(end = .);
}}
"""
    with open(filepath, 'w') as f:
        f.write(ld_content)

def main():
    parser = argparse.ArgumentParser(
        description="Generate PrimeShim assembly and linker input from a firmware ELF."
    )
    parser.add_argument("--elf", type=Path, default=DEFAULT_ELF_FILE)
    parser.add_argument("--map", type=Path, default=DEFAULT_MAP_FILE)
    parser.add_argument("--output-dir", type=Path, default=DEFAULT_FIRMWARE_DIR)
    args = parser.parse_args()

    args.output_dir.mkdir(parents=True, exist_ok=True)

    print(f"Loading SWI map from {args.map}...")
    swi_map = load_swi_map(args.map)

    print(f"Parsing ELF file {args.elf}...")
    try:
        with open(args.elf, 'rb') as f:
            elf = ELFFile(f)
            entry_addr = elf.header['e_entry']
            er_ro = elf.get_section_by_name('ER_RO')
            er_rw = elf.get_section_by_name('ER_RW')
            er_zi = elf.get_section_by_name('ER_ZI')

            if not all([er_ro, er_rw, er_zi]):
                print("Error: ELF file is missing one of ER_RO, ER_RW, or ER_ZI sections.")
                sys.exit(1)

            ro_addr = er_ro['sh_addr']
            ro_data = er_ro.data()

            rw_addr = er_rw['sh_addr']
            rw_data = er_rw.data()

            zi_addr = er_zi['sh_addr']
            zi_size = er_zi['sh_size']

    except Exception as e:
        print(f"Failed to read ELF: {e}")
        sys.exit(1)

    linker_path = args.output_dir / "linker.ld"
    text_path = args.output_dir / "text.asm"
    data_path = args.output_dir / "data.asm"

    print(f"Generating {linker_path}...")
    generate_linker_script(linker_path, ro_addr, rw_addr, zi_addr, zi_size)

    print(f"Generating {text_path}...")
    generate_asm(text_path, ro_data, ro_addr, '.ER_RO_text', is_text=True, swi_map=swi_map, entry_addr=entry_addr)

    print(f"Generating {data_path}...")
    generate_asm(data_path, rw_data, rw_addr, '.ER_RW_data', is_text=False, swi_map=swi_map)

    print("Done.")

if __name__ == '__main__':
    main()

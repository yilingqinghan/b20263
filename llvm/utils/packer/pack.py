#!/usr/bin/env python3
# -----------------------------------------------------------------------------
# NOTE: This file was created and/or refined with the assistance of AI tools
#       (e.g., ChatGPT). The author has reviewed and validated the content,
#       and remains fully responsible for its accuracy and completeness.
# -----------------------------------------------------------------------------
import sys, os, importlib
vendor = os.path.join(os.path.dirname(os.path.abspath(__file__)), "vendor")
sys.path.insert(0, vendor)
import zlib
def compress_payload(data: bytes) -> bytes:
    return zlib.compress(data, level=zlib.Z_BEST_COMPRESSION)
try:
    ext = importlib.import_module("zopfli.zopfli")
    import zopfli
    if hasattr(ext, "ZopfliCompressor"):
        zopfli.ZopfliCompressor = ext.ZopfliCompressor
    if hasattr(ext, "compress"):
        zopfli.compress = ext.compress
    zopfli.ZOPFLI_FORMAT_ZLIB = getattr(ext, "ZOPFLI_FORMAT_ZLIB", 1)
except ImportError:
    print("无法加载 Zopfli C-extension，后续使用纯 Python 或 zlib 回退")
try:
    import zopfli
    if hasattr(zopfli, 'ZopfliCompressor'):
        def compress_payload(data: bytes) -> bytes:
            comp = zopfli.ZopfliCompressor(
                zopfli.ZOPFLI_FORMAT_ZLIB, numiterations=180
            )
            return comp.compress(data) + comp.flush()
    elif hasattr(zopfli, 'compress'):
        def compress_payload(data: bytes) -> bytes:
            try:
                return zopfli.compress(data, zopfli.ZOPFLI_FORMAT_ZLIB, 180)
            except TypeError:
                return zopfli.compress(data, 180)
    else:
        try:
            from zopfli.zlib import compress as _zlib_compress
            def compress_payload(data: bytes) -> bytes:
                return _zlib_compress(data, 180)
        except ImportError:
            raise ImportError("no zopfli.compress and no zopfli.zlib")

except ImportError:
    import zlib
    def compress_payload(data: bytes) -> bytes:
        return zlib.compress(data, level=zlib.Z_BEST_COMPRESSION)

import struct, pathlib
from elftools.elf.elffile import ELFFile

if len(sys.argv) != 4:
    sys.exit(1)

stub_path, orig_path, packed_path = sys.argv[1:4]
out_dir = os.path.dirname(packed_path) or '.'
os.makedirs(out_dir, exist_ok=True)

stub_data = pathlib.Path(stub_path).read_bytes()

with open(orig_path, "rb") as f:
    elf = ELFFile(f)
    entry = elf.header["e_entry"]
    loads = [seg for seg in elf.iter_segments() if seg["p_type"] == "PT_LOAD"]

MAGIC = 0x52565058
hdr = struct.pack("<III", MAGIC, entry, len(loads))
payload_parts = []
payload_offset = 0
orig_bytes = pathlib.Path(orig_path).read_bytes()
for seg in loads:
    file_offset = seg["p_offset"]
    file_size = seg["p_filesz"]
    segment_data = orig_bytes[file_offset:file_offset + file_size]
    if len(segment_data) != file_size:
        sys.exit("PT_LOAD extends past end of input")
    hdr += struct.pack(
        "<IIIIII",
        seg["p_vaddr"], seg["p_filesz"], seg["p_memsz"],
        seg["p_flags"], seg["p_align"], payload_offset
    )
    payload_parts.append(segment_data)
    payload_offset += file_size

# The loader only copies PT_LOAD file contents into the mapped image. Keeping
# ELF section headers, relocation sections, and padding in the compressed blob
# wastes space without affecting execution.
payload = compress_payload(b"".join(payload_parts))

marker = b"\xDE\xAD\xBE\xEF"
marker_pos = stub_data.find(marker)
if marker_pos == -1:
    sys.exit("Error: Marker 0xDEADBEEF not found")
stub_data = (
        stub_data[:marker_pos]
        + struct.pack("<I", len(hdr))
        + stub_data[marker_pos+4:]
)

with open(packed_path, "wb") as f:
    f.write(stub_data + hdr + payload)

hdr_bin = os.path.join(out_dir, "hdr.bin")
payload_bin = os.path.join(out_dir, "payload.bin")
with open(hdr_bin,     "wb") as f: f.write(hdr)
with open(payload_bin, "wb") as f: f.write(payload)

def generate_c_file(bin_path, c_path, symbol):
    symbol_base = f"_binary_{symbol}"
    data        = pathlib.Path(bin_path).read_bytes()
    with open(c_path, "w") as f:
        f.write('#include <stddef.h>\n')
        f.write(f'__attribute__((used, aligned(1), visibility("default"))) '
                f'unsigned char {symbol_base}_start[] = {{\n')
        for i, b in enumerate(data):
            if i % 12 == 0:
                f.write("  ")
            f.write(f'0x{b:02x}, ')
            if (i+1) % 12 == 0:
                f.write("\n")
        f.write('\n};\n')
        f.write(
            f'__attribute__((used, visibility("default"))) '
            f'unsigned int {symbol_base}_len = {len(data)};\n'
        )

generate_c_file(hdr_bin,     os.path.join(out_dir, "hdr.c"),     "hdr_bin")
generate_c_file(payload_bin, os.path.join(out_dir, "payload.c"), "payload_bin")
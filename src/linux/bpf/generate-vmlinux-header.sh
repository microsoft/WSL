#!/bin/bash
set -e

if [ $# -lt 2 ]; then
    echo "Usage: $0 <bzImage> <output-vmlinux.h>"
    exit 1
fi

BZIMAGE="$1"
OUTPUT="$2"
BPFTOOL="${BPFTOOL:-bpftool}"

TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT

VMLINUX="$TMPDIR/vmlinux"

echo "Extracting vmlinux from $BZIMAGE..."

# First, try bpftool directly (works if input is already an ELF with BTF)
if "$BPFTOOL" btf dump file "$BZIMAGE" format c > "$OUTPUT" 2>/dev/null; then
    LINES=$(wc -l < "$OUTPUT")
    if [ "$LINES" -gt 100 ]; then
        echo "Done. Generated $OUTPUT ($LINES lines)"
        exit 0
    fi
fi

# Try to find an ELF embedded in the image (e.g., ARM64 Image)
ELF_OFFSET=$(binwalk -y elf "$BZIMAGE" 2>/dev/null | grep -oP '^\d+' | head -1) || true
if [ -n "$ELF_OFFSET" ]; then
    echo "Found ELF at offset $ELF_OFFSET"
    tail -c +$((ELF_OFFSET + 1)) "$BZIMAGE" > "$VMLINUX"
    if file "$VMLINUX" | grep -q 'ELF' && "$BPFTOOL" btf dump file "$VMLINUX" format c > "$OUTPUT" 2>/dev/null; then
        LINES=$(wc -l < "$OUTPUT")
        if [ "$LINES" -gt 100 ]; then
            echo "Done. Generated $OUTPUT ($LINES lines)"
            exit 0
        fi
    fi
fi

# Try to find raw BTF data in the image (ARM64 Image stores BTF as raw data)
BTF_RAW="$TMPDIR/btf.raw"
BTF_OFFSET=$(python3 -c "
import struct, sys
data = open(sys.argv[1], 'rb').read()
magic = b'\x9f\xeb'
idx = 0
while True:
    idx = data.find(magic, idx)
    if idx == -1: break
    if data[idx+2] == 1:  # version 1
        hdr_len = struct.unpack_from('<I', data, idx+4)[0]
        if hdr_len == 24:  # standard BTF header
            _, _, _, _, type_off, type_len, str_off, str_len = struct.unpack_from('<HBBI IIII', data, idx)
            total = hdr_len + type_len + str_len
            if total > 1000:
                print(f'{idx} {total}')
                break
    idx += 1
" "$BZIMAGE" 2>/dev/null) || true

if [ -n "$BTF_OFFSET" ]; then
    BTF_OFF=$(echo "$BTF_OFFSET" | awk '{print $1}')
    BTF_SIZE=$(echo "$BTF_OFFSET" | awk '{print $2}')
    echo "Found raw BTF at offset $BTF_OFF ($BTF_SIZE bytes)"
    tail -c +$((BTF_OFF + 1)) "$BZIMAGE" | head -c "$BTF_SIZE" > "$BTF_RAW"
    if "$BPFTOOL" btf dump file "$BTF_RAW" format c > "$OUTPUT" 2>/dev/null; then
        LINES=$(wc -l < "$OUTPUT")
        if [ "$LINES" -gt 100 ]; then
            echo "Done. Generated $OUTPUT ($LINES lines)"
            exit 0
        fi
    fi
fi

# Find gzip offset and decompress to get the vmlinux ELF
GZIP_OFFSET=$(binwalk -y gzip "$BZIMAGE" 2>/dev/null | grep -oP '^\d+' | head -1) || true

if [ -z "$GZIP_OFFSET" ]; then
    echo "Error: no gzip or ELF payload found in $BZIMAGE" >&2
    exit 1
fi

echo "Found gzip payload at offset $GZIP_OFFSET"
tail -c +$((GZIP_OFFSET + 1)) "$BZIMAGE" | zcat > "$VMLINUX" 2>/dev/null || true

if ! file "$VMLINUX" | grep -q 'ELF'; then
    # The gzip payload might contain another layer; try to find ELF inside
    INNER="$TMPDIR/inner"
    tail -c +$((GZIP_OFFSET + 1)) "$BZIMAGE" | zcat 2>/dev/null > "$INNER" || true

    # Search for ELF magic in decompressed data
    ELF_OFFSET=$(grep -a -b -o -P '\x7fELF' "$INNER" 2>/dev/null | head -1 | cut -d: -f1) || true
    if [ -n "$ELF_OFFSET" ]; then
        tail -c +$((ELF_OFFSET + 1)) "$INNER" > "$VMLINUX"
    fi
fi

if ! file "$VMLINUX" | grep -q 'ELF'; then
    echo "Error: could not extract a valid ELF vmlinux from $BZIMAGE" >&2
    exit 1
fi

echo "Extracted vmlinux: $(file "$VMLINUX")"
echo "Generating vmlinux.h with bpftool..."
"$BPFTOOL" btf dump file "$VMLINUX" format c > "$OUTPUT"

LINES=$(wc -l < "$OUTPUT")
echo "Done. Generated $OUTPUT ($LINES lines)"

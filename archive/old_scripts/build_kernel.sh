#!/bin/bash
# Kurono OS Enhanced Kernel - Full Build Script
# Produces a proper ELF32-i386 multiboot kernel from Cygwin toolchain
#
# FIXED STRATEGY:
# 1. Assemble boot with NASM -f win32 (COFF) -- same format as C++ objects
# 2. Compile C++ sources to COFF with -fno-leading-underscore
# 3. Link ALL COFF objects directly with ld -m i386pe (NO COFF→ELF pre-conversion)
# 4. Convert only the final PE to ELF32 with objcopy
#
# Fix for the +4 relocation bug:
# When converting individual COFF objects to ELF before linking, objcopy converts
# COFF DISP32 (formula: S+A-P-4) to ELF R_386_PC32 (formula: S+A-P) keeping A=0.
# This makes all calls jump to target+4 (wrong!). By keeping objects as COFF and
# only converting the final linked binary, we avoid any relocation corruption.
set -e

SRCDIR="/c/Users/genie/OS/src"
BUILDDIR="/c/Users/genie/OS/build_final"
BOOTDIR="/c/Users/genie/OS/BootArtifacts/EFI/KURONO"
OUTKERNEL="Kurono_kernel.elf"

# Compiler flags for bare-metal i386
# -fno-leading-underscore: make symbols without _ prefix to match NASM win32 refs
# -mno-stack-arg-probe: disable __chkstk_ms calls
# -fno-emutls: disable __emutls_get_address TLS emulation
CXXFLAGS="-m32 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
    -fno-exceptions -fno-rtti -fno-pie -fno-pic \
    -fno-leading-underscore -mno-stack-arg-probe \
    -O2 -Wall -Wno-unused-function -Wno-narrowing -Wno-unused-variable"
INCLUDES="-I${SRCDIR}/kernel -I${SRCDIR}/drivers -I${SRCDIR}"

echo "============================================="
echo " Kurono OS Enhanced 180Hz Kernel Build"
echo "============================================="

# Create build directory
rm -rf "$BUILDDIR"
mkdir -p "$BUILDDIR"

echo ""
echo "[1/5] Assembling multiboot boot loader..."
# Use -f win32 (COFF) so boot.o has the same format as C++ objects
# This avoids DISP32→R_386_PC32 addend corruption in objcopy
# With -f win32: symbol names are exactly as written (no auto underscore)
# With -fno-leading-underscore in C++: symbols match (both have no extra underscore)
nasm -f win32 -o "$BUILDDIR/boot.o" "$SRCDIR/boot/kurono_boot.asm"
echo "  -> boot.o (COFF/win32)"

echo ""
echo "[2/5] Compiling kernel C++ sources..."

CPP_SOURCES=(
    "kernel/types.cpp"
    "kernel/heap.cpp"
    "kernel/system.cpp"
    "kernel/time.cpp"
    "hal/hal.cpp"
    "drivers/serial.cpp"
    "drivers/timer.cpp"
    "drivers/rtc.cpp"
    "drivers/display.cpp"
    "drivers/graphics.cpp"
    "drivers/bga.cpp"
    "drivers/keyboard.cpp"
    "drivers/mouse.cpp"
    "fs/vfs.cpp"
    "proc/scheduler.cpp"
    "system/input_manager.cpp"
    "media/mediadecoder.cpp"
    "third_party/stb_image_glue.cpp"
    "third_party/stb_truetype_glue.cpp"
    "ui/font.cpp"
    "ui/gui.cpp"
    "ui/lockscreen.cpp"
    "ui/text_layout.cpp"
    "ui/ui_elements.cpp"
    "ui/file_browser.cpp"
    "tests/test_suite.cpp"
    "apps/calculator.cpp"
    "system/user_mgmt.cpp"
    "kernel/kurono_kernel.cpp"
)

OBJECT_FILES=()
FAIL=0

for src in "${CPP_SOURCES[@]}"; do
    srcpath="$SRCDIR/$src"
    if [ ! -f "$srcpath" ]; then
        echo "  SKIP: $src (not found)"
        continue
    fi

    objname=$(echo "$src" | sed 's|/|_|g' | sed 's|\.cpp$|.o|')
    objpath="$BUILDDIR/$objname"

    echo -n "  Compiling $src ... "
    if g++ $CXXFLAGS $INCLUDES -c "$srcpath" -o "$objpath" 2>"$BUILDDIR/${objname}.err"; then
        echo "OK"
        OBJECT_FILES+=("$objpath")
    else
        echo "FAILED"
        cat "$BUILDDIR/${objname}.err"
        FAIL=1
    fi
done

if [ "$FAIL" -eq 1 ]; then
    echo ""
    echo "*** COMPILE FAILED ***"
    for errfile in "$BUILDDIR"/*.err; do
        if [ -s "$errfile" ]; then
            echo "=== $(basename $errfile .err) ==="
            cat "$errfile"
        fi
    done
    exit 1
fi

echo ""
echo "[3/5] Providing runtime stubs..."

# Create stubs for missing runtime functions (__udivdi3, __chkstk_ms, __emutls)
cat > "$BUILDDIR/runtime_stubs.c" << 'STUBS'
// Bare-metal runtime stubs for Cygwin toolchain

typedef unsigned int uint32_t;
typedef unsigned long long uint64_t;
typedef int int32_t;
typedef long long int64_t;

// 64-bit unsigned division (needed for uint64_t / uint32_t)
uint64_t __udivdi3(uint64_t num, uint64_t den) {
    if (den == 0) return 0;
    if (den > num) return 0;
    if (den == 1) return num;

    // Simple shift-subtract division for 64-bit
    uint64_t quot = 0;
    int bits = 63;
    while (bits >= 0 && !((num >> bits) & 1)) bits--;

    uint64_t rem = 0;
    for (int i = bits; i >= 0; i--) {
        rem = (rem << 1) | ((num >> i) & 1);
        if (rem >= den) {
            rem -= den;
            quot |= (1ULL << i);
        }
    }
    return quot;
}

// 64-bit unsigned modulo
uint64_t __umoddi3(uint64_t num, uint64_t den) {
    return num - __udivdi3(num, den) * den;
}

// 64-bit signed division
int64_t __divdi3(int64_t a, int64_t b) {
    int neg = 0;
    if (a < 0) { a = -a; neg ^= 1; }
    if (b < 0) { b = -b; neg ^= 1; }
    int64_t result = (int64_t)__udivdi3((uint64_t)a, (uint64_t)b);
    return neg ? -result : result;
}

// 64-bit signed modulo
int64_t __moddi3(int64_t a, int64_t b) {
    return a - __divdi3(a, b) * b;
}

// Stack probing stub (Windows needs this for large stack frames)
void __chkstk_ms(void) {
    // On bare metal, we don't need stack probing - stack is pre-allocated
}

// Thread-local storage emulation stub (unused on bare metal)
void* __emutls_get_address(void* p) {
    // On bare metal single-threaded, just return the pointer as-is
    return p;
}
STUBS

echo -n "  Compiling runtime stubs ... "
gcc -m32 -ffreestanding -fno-builtin -fno-leading-underscore -fno-stack-protector \
    -mno-stack-arg-probe -c "$BUILDDIR/runtime_stubs.c" -o "$BUILDDIR/runtime_stubs.o" 2>&1
echo "OK"

echo ""
echo "[4/5] Linking kernel (all COFF objects directly → PE)..."

# All objects are COFF (boot.o from nasm -f win32, everything else from g++)
# Link them all directly as PE/COFF - NO pre-conversion to ELF needed
# This avoids the DISP32→R_386_PC32 addend bug that causes +4 offset on all calls

ALL_OBJECTS=("$BUILDDIR/boot.o")
for obj in "${OBJECT_FILES[@]}"; do
    ALL_OBJECTS+=("$obj")
done
ALL_OBJECTS+=("$BUILDDIR/runtime_stubs.o")

# Create linker script
cat > "$BUILDDIR/link.ld" << 'LINKERSCRIPT'
ENTRY(_start)

SECTIONS
{
    . = 0x100000;

    /* Multiboot header must be within first 8KB */
    .mboot ALIGN(4) : {
        *(.mboot)
        *(.multiboot)
    }

    .text ALIGN(16) : {
        *(.text)
        *(.text.*)
    }

    .rodata ALIGN(4K) : {
        *(.rodata)
        *(.rodata.*)
        *(.rdata)
        *(.rdata.*)
    }

    .eh_frame ALIGN(4) : {
        *(.eh_frame)
    }

    .data ALIGN(4K) : {
        *(.data)
        *(.data.*)
        *(.got)
        *(.got.plt)
    }

    .bss ALIGN(4K) : {
        kernel_bss_start = .;
        *(COMMON)
        *(.bss)
        *(.bss.*)
        kernel_bss_end = .;
    }

    /* Stack lives AFTER BSS so clearing BSS doesn't wipe pushed args */
    .stk ALIGN(4K) : {
        *(.stk)
        *(.bootstrap_stack)
    }

    . = ALIGN(4K);
    kernel_end = .;

    /DISCARD/ : {
        *(.comment)
        *(.note.*)
        *(.drectve)
        *(.idata)
        *(.ctors)
        *(.dtors)
        *(.CRT*)
        *(.tls)
        *(.tbss)
    }
}
LINKERSCRIPT

echo "  Linking ${#ALL_OBJECTS[@]} COFF object files..."

ld -m i386pe \
    -e _start \
    -T "$BUILDDIR/link.ld" \
    -o "$BUILDDIR/kernel.pe" \
    "${ALL_OBJECTS[@]}" 2>"$BUILDDIR/link.log"

LINK_EXIT=$?
if [ $LINK_EXIT -ne 0 ]; then
    echo "  Link warnings/errors:"
    cat "$BUILDDIR/link.log"
    if [ ! -f "$BUILDDIR/kernel.pe" ]; then
        echo "  *** LINK FAILED - no output ***"
        exit 1
    fi
    echo "  (continuing despite warnings - output was created)"
fi

if [ -s "$BUILDDIR/link.log" ]; then
    echo "  Link notes:"
    cat "$BUILDDIR/link.log" | head -5
fi

PE_SIZE=$(stat -c%s "$BUILDDIR/kernel.pe")
echo "  -> kernel.pe ($PE_SIZE bytes)"

echo ""
echo "[5/5] Converting PE to ELF32 & deploying..."
# Convert final PE to ELF32 - this is safe because all relocations are already
# resolved in the PE (no relocation entries needed in ELF output)
objcopy -O elf32-i386 "$BUILDDIR/kernel.pe" "$BUILDDIR/$OUTKERNEL"

# Verify multiboot header
echo -n "  Multiboot header: "
if hexdump -C "$BUILDDIR/$OUTKERNEL" | grep -q "02 b0 ad 1b"; then
    echo "FOUND"
else
    echo "WARNING - not found!"
fi

# Verify entry point
echo -n "  Entry point: "
readelf -h "$BUILDDIR/$OUTKERNEL" 2>/dev/null | grep "Entry" || echo "unknown"

# Verify it's proper ELF
echo -n "  Format: "
file -b "$BUILDDIR/$OUTKERNEL"

KERNEL_SIZE=$(stat -c%s "$BUILDDIR/$OUTKERNEL")
echo "  Size: $KERNEL_SIZE bytes"

# Deploy
cp "$BUILDDIR/$OUTKERNEL" "$BOOTDIR/$OUTKERNEL"
echo "  Deployed to: $BOOTDIR/$OUTKERNEL"

echo ""
echo "============================================="
echo " BUILD SUCCESS"
echo "============================================="
echo ""
echo "Run with:"
echo "  C:/msys64/mingw64/bin/qemu-system-i386.exe \\"
echo "    -kernel '$BOOTDIR/$OUTKERNEL' \\"
echo "    -m 256M -vga std -serial stdio"
echo ""
echo "Run with boot logo & wallpaper:"
echo "  C:/msys64/mingw64/bin/qemu-system-i386.exe \\"
echo "    -kernel '$BOOTDIR/$OUTKERNEL' \\"
echo "    -initrd '$BOOTDIR/logo.png logo,$BOOTDIR/wallpaper.png wallpaper,$BOOTDIR/font.ttf font' \\"
echo "    -m 256M -vga std -serial stdio"

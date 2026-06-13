# KFS  -  Kurono File System

`src/fs/kfs.cpp` and `kfs.h` implement KFS, a small, fast, inode-based on-disk
filesystem designed from scratch for Kurono  -  no Linux baggage. It is the
**on-disk persistence layer**: the in-memory [KVFS](KVFS.md) stays the runtime
filesystem, and KFS stores the user-data subset across reboots as **real files +
directories** (not an opaque blob), so a volume is browsable and a future Linux
`kfs-fuse` driver could mount it. The on-disk format below is the spec for that.

## 1. Why a new filesystem

Persistence used to serialize the KVFS tree into a single CRC-checked raw-sector
blob (`PersistStore::Save`/`Load`). That worked, but the data disk was opaque  - 
nothing but Kurono could read it, and there were no real files. KFS replaces the
blob with a genuine filesystem while keeping the same selective-snapshot policy.

## 2. On-disk layout

4 KB blocks; all multi-byte fields little-endian.

| Blocks | Contents |
| --- | --- |
| `0` | Superblock (`KFSSuper`) |
| `1 .. B` | Free-block bitmap  -  1 bit/block, bit set = used |
| `B+1 .. I` | Inode table  -  32 inodes/block, 128 B each (`KFSInode`) |
| `I+1 ..` | Data blocks |

**Superblock** (`KFSSuper`, block 0): magic `0x4B465331` ("KFS1"), version,
block size, total/free block counts, and the start block + length of the bitmap
and inode regions, plus a CRC32 over those fields. `Mount` validates the magic
and CRC before trusting anything.

> **KFS v2 (extent-based)  -  current on-disk format.** As of the performance +
> capability overhaul the magic is `"KFS2"` (`0x4B465332`), the inode is **256
> bytes**, and files/dirs are described by **extents**, not per-block pointers.
> The v1 (`"KFS1"`, 128-byte inode, 13-direct + 1-indirect) layout below is kept
> for historical reference; the §7 FUSE byte-offset tables still describe v1 and
> need a v2 refresh. See **§10 Limits removed** and **§11 Optimization layers +
> benchmarks** for the v2 format and numbers.

**Inode** (`KFSInode`, exactly 128 bytes in v1 / **256 bytes in v2**): type
(`FREE`/`FILE`/`DIR`/`SYMLINK`), unix `mode`/`uid`/`gid`, `size`, c/m/a-times,
`nlink`. **v1** used **13 direct** block pointers + **1 single-indirect** pointer
(a block of 1024 pointers), capping a file at `(13 + 1024) × 4 KB ≈ 4.05 MB`
(`KFS_MAX_FILE`). **v2 removed that cap**: the inode holds **23 inline extents**
`{start_lba, len_blocks}` plus an **unbounded overflow-block chain** (511 extents
per 4 KB block), so a file is limited only by free disk space and a contiguous
file is a **single extent** (a 174 MB binary = 1 extent, not 43520 pointers).
Tiny files (`≤ 184 B`) are stored **inline in the inode** with no data block.
Inode 0 is reserved (means "none"); **inode 1 is the root directory**.

**Directory entry** (`KFSDirEnt`, exactly 64 bytes): child inode number,
`name_len`, a type hint for listings, and up to a 55-char name. A directory's
data blocks are just arrays of these (64 entries/block); `inode == 0` marks a
free slot.

## 3. Device-agnostic backend

KFS never touches hardware directly. The caller wires two callbacks via
`KFS::SetBackend(rd, wr, ctx)`:

```c
bool read (uint64_t block, uint32_t count, void*       buf, void* ctx);
bool write(uint64_t block, uint32_t count, const void* buf, void* ctx);
```

The kernel wires these to the NVMe driver (block × 4 KB → LBA; see
[NVME.md](../drivers/NVME.md)). A host `kfs-fuse` driver would wire them to a
file. This is the seam that makes the on-disk format portable.

## 4. Allocation & the snapshot model

Persistence formats a **fresh** volume on every save (`PersistStore::SaveTree`),
so KFS uses a simple **bump allocator**: a monotonically rising next-free pointer
hands out contiguous runs. Because each file's blocks are contiguous and the
in-kernel DMA buffers are identity-mapped (physically contiguous), a whole file
can be read or written in **one multi-page NVMe command** rather than a page at a
time  -  this is what made the two-boot persistence test complete within the boot
window. The bitmap and inode table are cached in RAM during a session and flushed
in `Sync()` after a batch of writes.

**Verified speed.** With the headless two-boot test (`kurono.kfstest`, see §7), a
save of the user-data tree (≈ 0.95 MB including a 256 KB test file) goes to disk
in **46 multi-page NVMe commands and ~5 ms**; the old single-page path
(4 KB / command, `prp1` only) would have needed **237 commands** for the same
bytes. The restore (mount + walk + rebuild KVFS) completes in **~4 ms**. The 256 KB
file's every byte round-trips correctly across the reboot.

This snapshot model trades in-place mutation for simplicity and speed: there is
no free-list reuse or fragmentation handling, because each save starts clean.

## 5. API

`SetBackend` → `Format(total_blocks)` (lay down an empty volume, create root) or
`Mount()` (validate + load an existing one), then path ops on absolute
`/`-separated paths: `Mkdirs`, `WriteFile`, `ReadFile`, `Exists`, `IsDir`,
`FileSize`, and `List` (per-child callback, used by the restore walk).
`Sync()` flushes dirty metadata. `IsMounted()` reports state.

## 6. Use in the OS

KFS is driven entirely by `PersistStore` (`src/fs/persist.cpp`):

- **Save** (`SaveTree`): `Format` a fresh volume, then recursively mirror the
  user-data subtrees `/home`, `/etc`, `/root` from KVFS into KFS as real files +
  dirs (skipping files over `KFS_MAX_FILE` and the re-seeded `/usr` binaries),
  then `Sync`.
- **Restore** (`LoadTree`): `Mount`, then walk the volume with `List` and rebuild
  the KVFS tree before boot seeding re-fills the large `/usr` binaries.

See [KVFS.md](KVFS.md) §4 for how this fits the overall persistence flow.

## 7. FUSE-compatible on-disk spec

KFS is intentionally simple enough to mount on Linux with a small FUSE driver.
The layout below is the complete, authoritative format  -  a `kfs-fuse` driver only
has to read it; it never needs Kurono-internal state.

**Conventions.** All multi-byte integers are **little-endian**. The block size is
**4096 bytes** (`block_size` in the superblock; reject anything else). All
structures are tightly packed (`__attribute__((packed))`); offsets below are exact
byte offsets within the structure. Block numbers are absolute, 0-based 4 KB block
indices into the device.

**Superblock  -  block 0** (`KFSSuper`, fields in order):

| Off | Type | Field | Notes |
| --- | --- | --- | --- |
| 0 | u32 | `magic` | `0x4B465331` ("KFS1") |
| 4 | u32 | `version` | `1` |
| 8 | u32 | `block_size` | `4096` |
| 12 | u32 | `total_blocks` | volume size in blocks |
| 16 | u32 | `inode_count` | number of inode slots |
| 20 | u32 | `bitmap_start` | first bitmap block (always `1`) |
| 24 | u32 | `bitmap_blocks` | bitmap length in blocks |
| 28 | u32 | `inode_start` | first inode-table block |
| 32 | u32 | `inode_blocks` | inode-table length in blocks |
| 36 | u32 | `data_start` | first data block |
| 40 | u32 | `free_blocks` | running free count (advisory) |
| 44 | u32 | `crc` | CRC32 of bytes `[0, 44)` |

`crc` is the standard CRC32 (reflected, polynomial `0xEDB88320`, init/xor-out
`0xFFFFFFFF`) over the 44 bytes preceding it. A mounter validates `magic`,
`version`, `block_size`, then `crc`.

**Free-block bitmap  -  blocks `bitmap_start .. bitmap_start+bitmap_blocks-1`.**
1 bit per block, LSB-first within each byte: block `b`'s bit is
`bitmap[b >> 3] & (1 << (b & 7))`. Bit **set = used**. Metadata blocks
(`0 .. data_start-1`) are marked used at format time. The bitmap is advisory for a
read-only FUSE driver (file extents are found through inodes), but a read-write
driver must honor it.

**Inode table  -  blocks `inode_start ..`,** 32 inodes per block, each exactly **128
bytes** (`KFS_INODE_SIZE`). Inode number `n` lives at byte offset `n * 128` from
`inode_start`'s base. **Inode 0 is reserved** (`type == FREE`, means "no inode").
**Inode 1 is the root directory.** `KFSInode` fields:

| Off | Type | Field | Notes |
| --- | --- | --- | --- |
| 0 | u32 | `type` | `0`=free, `1`=file, `2`=dir |
| 4 | u16 | `mode` | unix `rwxrwxrwx` |
| 6 | u16 | `uid` |  |
| 8 | u16 | `gid` |  |
| 10 | u16 | `_pad0` | 0 |
| 12 | u32 | `size` | bytes (file) / dir-data bytes (dir) |
| 16 | u32 | `ctime` | seconds (boot-relative in this build) |
| 20 | u32 | `mtime` |  |
| 24 | u32 | `atime` |  |
| 28 | u32 | `nlink` |  |
| 32 | u32×13 | `direct[13]` | data block numbers; `0` = unused |
| 84 | u32 | `indirect` | block of up to 1024 u32 block numbers |
| 88 | u32×8 | `_pad1[8]` | reserved → 128 bytes |

A file's data blocks are `direct[0..12]` then, if `size` needs more than 13
blocks, the `indirect` block holds the remaining block numbers (1024 u32 entries,
little-endian). Max file size is `(13 + 1024) × 4096 ≈ 4.05 MB`. In the current
snapshot writer every file occupies one **contiguous** run starting at `direct[0]`,
but a conformant reader must not assume contiguity  -  it must follow the pointers.

**Directory data** is an array of **64-byte** `KFSDirEnt` entries (64 per block),
stored in the directory inode's data blocks:

| Off | Type | Field | Notes |
| --- | --- | --- | --- |
| 0 | u32 | `inode` | child inode number; `0` = empty slot |
| 4 | u16 | `name_len` | name length (≤ 55) |
| 6 | u16 | `type` | listing hint (`1`=file, `2`=dir) |
| 8 | char[56] | `name` | NUL-terminated, up to 55 chars |

To enumerate a directory, read its data blocks and emit every entry with
`inode != 0`. There are no `.`/`..` entries on disk  -  a FUSE driver synthesizes
them. The current writer uses only the 13 direct blocks for directory data (it
never grows a directory past `13 × 64 = 832` entries); a reader should still walk
`indirect` for forward compatibility.

A minimal read-only `kfs-fuse`: read the superblock, validate it, read the inode
table (and bitmap) into memory, then implement `getattr`/`readdir`/`read` by
resolving paths through `dir` inodes and reading file extents via `direct` +
`indirect`. No journaling or in-place writes are required for read-only use.

## 8. Headless two-boot test

The whole persist/restore loop is exercised without an interactive shell via the
`kurono.kfstest` kernel cmdline token (parsed in `kurono_kernel.cpp` alongside the
other `kurono.*` tokens; the hook runs right after the boot-time `LoadTree`):

- **Boot 1** (no marker present after restore): writes `/home/user/.kfstest/marker`,
  a deep nested `/etc/kfstest/deep/nested/file.txt`, and a 256 KB
  `/home/user/.kfstest/big.bin` (byte pattern `1 + (i & 0xff)`), times
  `PersistStore::SaveTree()`, logs the byte/command counts (`[KFSTEST] boot1 nvme
  write: ...`), then powers off.
- **Boot 2** (same disk): the boot-time restore rebuilds the tree, so the marker is
  present; the hook verifies the marker string, the deep path's content, and every
  byte of `big.bin`, logs `[KFSTEST] PASS`/`FAIL` and the restore time, then powers
  off.

Run it headless (BIOS path, fresh disk for boot 1, same disk for boot 2):

```
qemu-img create -f raw /tmp/kfs_test.img 256M
qemu-system-x86_64 -machine pc,vmport=off -cpu host -smp 4 -m 4G \
  -cdrom build/kurono.iso -vga virtio -no-reboot -enable-kvm \
  -drive file=/tmp/kfs_test.img,if=none,id=kdata,format=raw \
  -device nvme,serial=kuronodata,drive=kdata \
  -display none -serial file:/tmp/kfs_serial.log
```

with a grub entry that appends `kurono.text=1 kurono.kfstest=1`. Boot twice; the
second run logs `[KFSTEST] PASS`.

## 9. Related files

- `src/fs/kfs.h`  -  on-disk format spec + API (the authoritative reference)
- `src/fs/kfs.cpp`  -  KFS v2 (extent-based) implementation
- `src/fs/kfs_bench.{h,cpp}`  -  headless storage benchmark (`kurono.kfsbench`)
- `src/fs/persist.cpp`  -  `PersistStore`, the only KFS caller
- `src/fs/kvfs.cpp`  -  the in-memory runtime filesystem KFS backs
- `src/drivers/nvme.cpp`  -  the block device under KFS

## 10. Limits removed (v2 capability overhaul)

Every hard cap that existed in v1 was removed:

| Limit (v1) | v1 value | v2 |
| --- | --- | --- |
| Max file size | `(13 + 1024) × 4 KB ≈ 4.05 MB` (`KFS_MAX_FILE`) | **none**  -  extents; limited only by free disk |
| Max directory entries | `13 × 64 = 832` (13 direct dir blocks) | **none**  -  dirs grow via the same extents |
| Max volume size | `65536` blocks = **256 MB** (`kfs_disk_blocks`) | **full NVMe capacity** (clamped only by a 512 MB metadata-cache budget ≈ 228 GB, and the u32 block-number space) |
| Max path length | `256` (KFS internal `parent_path`) | **4096** (Linux `PATH_MAX`), heap-backed |
| Hardcoded buffers | 512-child save / 256-entry restore | **dynamic** (sized to the actual directory) |

The persistence layer no longer skips "oversized" files  -  it mirrors whatever
KVFS holds. Symlinks are now a first-class KFS node type (`KFS::Symlink` /
`ReadLink`) so the Linux-compat overlay round-trips through persistence.

## 11. Optimization layers + benchmarks

Measured headless via `kurono.kfsbench` (`src/fs/kfs_bench.cpp`): a fresh KFS
volume on a dedicated 1 GB NVMe data disk, 32 MB sequential file, under
**QEMU 10 + KVM**, `-cpu host -smp 4 -m 4G`, 2880 MHz TSC. **All timing is
rdtsc-based.** Run with:

```
qemu-img create -f raw /tmp/kfsopt.img 1G
# grub entry appends: kurono.text=1 kurono.kfsbench=1   (destructive to the disk)
qemu-system-x86_64 -machine pc,vmport=off -cpu host -smp 4 -m 4G \
  -cdrom build/kurono.iso -vga virtio -no-reboot -enable-kvm \
  -drive file=/tmp/kfsopt.img,if=none,id=kdata,format=raw \
  -device nvme,serial=kuronodata,drive=kdata -display none -serial stdio
```

**Honest caveat on the QEMU/KVM numbers.** The emulated NVMe is host-RAM-backed,
so sequential throughput is bounded by the bounce-buffer **memcpy + QEMU
emulation overhead**, not by disk or NVMe queue latency. Per-run variance is
**±15 - 20%**, which is large. Numbers below are the median of 5 runs; treat
single-digit-percent diffs as noise.

| Metric | v1 (pre-overhaul, single-page DMA*) | v2 all layers (median of 5) |
| --- | --- | --- |
| Sequential write | ~14 MB/s (the old framebuffer-era path) | **~889 MB/s** |
| Sequential read | n/a (blob) | **~2556 MB/s** |
| Random 4K write IOPS (raw NVMe) |  -  | **~38 000** |
| Snapshot save (full, 32 MB) |  -  | **~36.7 ms** |
| Snapshot save (incremental, no change) |  -  (always full) | **~1 ms, 0 NVMe write cmds** |
| Boot restore (mount + read 32 MB) |  -  | **~14.6 ms** |
| 32 MB file extent count | 8192 block pointers | **1 extent** |
| 512 tiny files (≤184 B) | 512 data blocks | **0 data blocks** (all inline) |

\* The multi-page-DMA `prp` work that predates this overhaul already lifted the
old 4 KB-per-command path; e.g. the two-boot test moves 1.45 MB in **35
multi-page commands** vs **354** on the old single-page path.

### Layers implemented

- **Layer 4  -  extent layout (done).** Files/dirs are extent lists; the bump
  allocator yields one contiguous run, so a file is one extent. This is the
  mechanism that removes the file-size + dir-entry caps. Verified: a 32 MB file
  = 1 file extent; reads coalesce adjacent extents into one ≤2 MB NVMe command.

- **Layer 1  -  NVMe queue-depth (done, honest).** `NVMe::Read`/`Write` now batch
  up to 32 ≤2 MB chunks per submission-queue doorbell ring and reap all
  completions together (vs the old submit-one/poll-one QD=1), and one I/O queue
  pair is created **per CPU core** (QID 1..4), routed by `SMP::CpuIndex()`.
  Architecturally correct and verified at boot (`4 i/o queue pair(s) created`),
  **but** on the RAM-backed QEMU NVMe the win is within the ±15 - 20% run-to-run
  noise  -  the bottleneck there is memcpy/emulation, not doorbell round-trips.
  This is the right primitive for real hardware (deep queues, true per-core
  concurrency); multi-queue's per-core benefit is also latent until the
  cooperative scheduler permits concurrent submitters.

- **Layer 6  -  incremental snapshot (done).** `SaveTree` fingerprints the
  user-data set (64-bit FNV over path/type/size/content), persists it in the
  superblock, and **skips the entire reformat+rewrite when nothing changed**.
  Verified: re-saving an unchanged tree drops from **6 ms / 35 NVMe write cmds**
  to **1 ms / 0 cmds**. Tiny files (≤184 B) are stored **inline in the inode**
  (Layer 6 "inline-compress small files")  -  512/512 tiny files used 0 data
  blocks.

### Layers NOT implemented (honest scope)

Layers 2 (write-back page cache), 3 (log-structured write path + NAT + cleaner),
5 (read-ahead/prefetch), and 7 (NVMe FDP placement groups) were **not** built.
They presuppose a **mutable, mounted** filesystem with random in-place writes;
KFS today is a **snapshot** filesystem (each `SaveTree` formats a fresh volume
and writes contiguous runs), so those layers would require a different on-disk
model than the one that makes the snapshot path fast and simple. They are the
right next step if/when KFS becomes the live runtime FS rather than the
periodic-snapshot persistence layer.

### Comparison to ext4 / NTFS

An honest comparison is **structural, not a head-to-head throughput shoot-out**  - 
the only block device available here is QEMU's RAM-backed NVMe, on which any
filesystem is memcpy-bound, so a MB/s race against ext4 would measure QEMU, not
the filesystems. On capabilities and the workload that matters for Kurono
(snapshotting the user-data tree across reboots):

- **Extents.** KFS v2 uses extents like ext4 and NTFS (vs ext2/FAT block
  pointers). A 174 MB file is 1 extent in all three; KFS reaches it through ≤23
  inline + an overflow chain, ext4 through an extent tree, NTFS through runlists.
- **Inline small files.** KFS stores ≤184 B files in the inode (0 data blocks),
  like NTFS resident files and ext4 inline_data. A 512-file tiny-file set costs
  0 data blocks in KFS.
- **Contiguity.** KFS's snapshot bump-allocator guarantees a fresh file is
  perfectly contiguous (1 extent, no fragmentation)  -  something ext4/NTFS only
  approximate after use because they allocate in a live, fragmented free space.
- **Incremental snapshot.** "Save nothing if nothing changed" (1 ms / 0 writes)
  has no direct ext4/NTFS equivalent at the volume level; it is closer to a
  copy-on-write snapshot (Btrfs/ZFS/VSS) but coarser (whole-tree fingerprint).
- **What KFS does NOT have vs ext4/NTFS:** journaling, in-place random writes, a
  write-back page cache, prefetch, ACLs/xattrs, online resize, fsck. These are
  the unbuilt Layers 2/3/5 plus general maturity. KFS is a fast, simple,
  unbounded **snapshot** store  -  not yet a general-purpose mutable FS.

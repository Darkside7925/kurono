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

**Inode** (`KFSInode`, exactly 128 bytes): type (`FREE`/`FILE`/`DIR`), unix
`mode`/`uid`/`gid`, `size`, c/m/a-times, `nlink`, **13 direct** block pointers
and **1 single-indirect** pointer (a block of 1024 pointers). That gives a max
file size of `(13 + 1024) × 4 KB ≈ 4.05 MB` (`KFS_MAX_FILE`), which is the cap
the persistence layer also uses to skip oversized re-seeded media. Inode 0 is
reserved (means "none"); **inode 1 is the root directory**.

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

## 7. Related files

- `src/fs/kfs.h`  -  on-disk format spec + API (the authoritative reference)
- `src/fs/persist.cpp`  -  `PersistStore`, the only KFS caller
- `src/fs/kvfs.cpp`  -  the in-memory runtime filesystem KFS backs
- `src/drivers/nvme.cpp`  -  the block device under KFS

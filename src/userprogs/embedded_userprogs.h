#pragma once
#include "../system/kasset.h"   // big payloads load on demand (satoru)
//  Embedded user-mode test program(s).  Linked into the kernel image via
//  objcopy from src/userprogs/<name>.asm \u2192 elf64 \u2192 binary blob.

#include "../kernel/types.h"

extern "C" {
    extern const uint8_t _binary_hello_elf_bin_start[] __attribute__((weak));
    extern const uint8_t _binary_hello_elf_bin_end[]   __attribute__((weak));
    extern const uint8_t _binary_hello_x64_elf_bin_start[] __attribute__((weak));
    extern const uint8_t _binary_hello_x64_elf_bin_end[]   __attribute__((weak));
    extern const uint8_t _binary_kpython_elf_bin_start[]   __attribute__((weak));
    extern const uint8_t _binary_kpython_elf_bin_end[]     __attribute__((weak));
    // musl-static hello blob (satoru)
    extern const uint8_t _binary_hello_musl_elf_start[]    __attribute__((weak));
    extern const uint8_t _binary_hello_musl_elf_end[]      __attribute__((weak));
    // musl-static ffmpeg blob (satoru)
    extern const uint8_t _binary_ffmpeg_elf_start[]        __attribute__((weak));
    extern const uint8_t _binary_ffmpeg_elf_end[]          __attribute__((weak));
    // raw-protocol wl_shm wayland test client blob (satoru)
    extern const uint8_t _binary_wl_shm_test_elf_start[]   __attribute__((weak));
    extern const uint8_t _binary_wl_shm_test_elf_end[]     __attribute__((weak));
    // pthreads smoke-test blob (satoru)
    extern const uint8_t _binary_pthread_test_elf_start[]  __attribute__((weak));
    extern const uint8_t _binary_pthread_test_elf_end[]    __attribute__((weak));
    // L2 mmap hole reuse test blob (satoru)
    extern const uint8_t _binary_mmap_reuse_test_elf_start[] __attribute__((weak));
    extern const uint8_t _binary_mmap_reuse_test_elf_end[]   __attribute__((weak));
    // signal-delivery self-test blob (satoru)
    extern const uint8_t _binary_kld_e1000e_elf_start[]    __attribute__((weak));
    extern const uint8_t _binary_kld_e1000e_elf_end[]      __attribute__((weak));
    extern const uint8_t _binary_kld_apitest_elf_start[]   __attribute__((weak));
    extern const uint8_t _binary_kld_apitest_elf_end[]     __attribute__((weak));
    extern const uint8_t _binary_kld_iwlwifi_elf_start[]    __attribute__((weak));
    extern const uint8_t _binary_kld_iwlwifi_elf_end[]      __attribute__((weak));
    extern const uint8_t _binary_uatest_elf_start[]        __attribute__((weak));
    extern const uint8_t _binary_kentest_elf_start[]      __attribute__((weak));
    extern const uint8_t _binary_kentest_elf_end[]        __attribute__((weak));
    // /dev/kshell gate blob (kurono.kshtest=1) (satoru)
    extern const uint8_t _binary_kshtest_elf_start[]       __attribute__((weak));
    // ksh-run: the linux-side /dev/kshell client (claude's bash bridge) (satoru)
    extern const uint8_t _binary_kshrun_elf_start[]        __attribute__((weak));
    extern const uint8_t _binary_kshrun_elf_end[]          __attribute__((weak));
    // a real bash (static) for claude's bash tool, optional blob (satoru)
    extern const uint8_t _binary_bash_elf_start[]          __attribute__((weak));
    extern const uint8_t _binary_bash_elf_end[]            __attribute__((weak));
    // static musl git, optional blob (satoru)
    extern const uint8_t _binary_git_elf_start[]           __attribute__((weak));
    extern const uint8_t _binary_git_elf_end[]             __attribute__((weak));
    // prebuilt static busybox - third-party linux binary smoke test (satoru)
    extern const uint8_t _binary_busybox_elf_start[]       __attribute__((weak));
    extern const uint8_t _binary_busybox_elf_end[]         __attribute__((weak));
    // ai recovery client + optional baked api key (kurono.dsai=1) (satoru)
    extern const uint8_t _binary_dsai_elf_start[]          __attribute__((weak));
    extern const uint8_t _binary_dsai_elf_end[]            __attribute__((weak));
    extern const uint8_t _binary_dsai_key_bin_start[]      __attribute__((weak));
    extern const uint8_t _binary_dsai_key_bin_end[]        __attribute__((weak));
    // https downloader for kpkg (kpkg install claude) (satoru)
    extern const uint8_t _binary_kfetch_elf_start[]        __attribute__((weak));
    extern const uint8_t _binary_kfetch_elf_end[]          __attribute__((weak));
    // recovery toolbox: a ustar of musl-static rescue tools (satoru)
    extern const uint8_t _binary_recovery_tools_tar_start[] __attribute__((weak));
    extern const uint8_t _binary_recovery_tools_tar_end[]   __attribute__((weak));
    extern const uint8_t _binary_kshtest_elf_end[]         __attribute__((weak));
    extern const uint8_t _binary_uatest_elf_end[]          __attribute__((weak));
    // ring-3 audio daemon (kurono.r3audio=1) (satoru)
    extern const uint8_t _binary_audiod_elf_start[]        __attribute__((weak));
    extern const uint8_t _binary_audiod_elf_end[]          __attribute__((weak));
    // /system/bin/kaudio: audio restart/stop/status through /dev/kaudio (satoru)
    extern const uint8_t _binary_kaudio_elf_start[]        __attribute__((weak));
    extern const uint8_t _binary_kaudio_elf_end[]          __attribute__((weak));
    extern const uint8_t _binary_irqlat_elf_start[]        __attribute__((weak));
    extern const uint8_t _binary_irqlat_elf_end[]          __attribute__((weak));
    extern const uint8_t _binary_smpscale_elf_start[]      __attribute__((weak));
    extern const uint8_t _binary_smpscale_elf_end[]        __attribute__((weak));
    // posix_spawn static-elf64 repro blob (satoru)
    extern const uint8_t _binary_spawntest_elf_start[]     __attribute__((weak));
    extern const uint8_t _binary_spawntest_elf_end[]       __attribute__((weak));
    extern const uint8_t _binary_kldnetbench_elf_start[]   __attribute__((weak));   // (satoru)
    extern const uint8_t _binary_kldnetbench_elf_end[]     __attribute__((weak));   // (satoru)
    extern const uint8_t _binary_kld_acpi_elf_start[]      __attribute__((weak));   // (satoru)
    extern const uint8_t _binary_kld_acpi_elf_end[]        __attribute__((weak));   // (satoru)
    extern const uint8_t _binary_kld_i2chid_elf_start[]    __attribute__((weak));   // (satoru)
    extern const uint8_t _binary_kld_i2chid_elf_end[]      __attribute__((weak));   // (satoru)
    extern const uint8_t _binary_signal_test_elf_start[]   __attribute__((weak));
    extern const uint8_t _binary_signal_test_elf_end[]     __attribute__((weak));
    // interruptible-sleep self-test blob (satoru)
    extern const uint8_t _binary_sleeptest_elf_start[]     __attribute__((weak));
    extern const uint8_t _binary_sleeptest_elf_end[]       __attribute__((weak));
    // close-on-exec + unix socket self-test blob (satoru)
    extern const uint8_t _binary_fdtest_elf_start[]        __attribute__((weak));
    extern const uint8_t _binary_fdtest_elf_end[]          __attribute__((weak));
    // parallel tree-walk test blob (kurono.findtest=1) (satoru)
    extern const uint8_t _binary_findtest_elf_start[]      __attribute__((weak));
    extern const uint8_t _binary_findtest_elf_end[]        __attribute__((weak));
    // dynamic musl pie blob - first ld-kurono dynamic-load test (satoru)
    extern const uint8_t _binary_dyntest_elf_start[]       __attribute__((weak));
    extern const uint8_t _binary_dyntest_elf_end[]         __attribute__((weak));
    // musl libc.so blob - satisfies the pie's DT_NEEDED (satoru)
    extern const uint8_t _binary_musl_libc_so_start[]      __attribute__((weak));
    extern const uint8_t _binary_musl_libc_so_end[]        __attribute__((weak));
    // libfoo.so blob - the dyntest pie's extra dependency (satoru)
    extern const uint8_t _binary_libfoo_so_start[]         __attribute__((weak));
    extern const uint8_t _binary_libfoo_so_end[]           __attribute__((weak));
}

namespace EmbeddedUserprogs {

inline bool HasHello() {
    return _binary_hello_elf_bin_start != nullptr &&
           _binary_hello_elf_bin_end   != nullptr &&
           +_binary_hello_elf_bin_end > +_binary_hello_elf_bin_start;
}

inline const uint8_t* HelloData() {
    return _binary_hello_elf_bin_start;
}

inline uint32_t HelloSize() {
    return (uint32_t)((uintptr_t)_binary_hello_elf_bin_end -
                      (uintptr_t)_binary_hello_elf_bin_start);
}

inline bool HasHelloX64() {
    return _binary_hello_x64_elf_bin_start != nullptr &&
           _binary_hello_x64_elf_bin_end   != nullptr &&
           +_binary_hello_x64_elf_bin_end > +_binary_hello_x64_elf_bin_start;
}

inline const uint8_t* HelloX64Data() {
    return _binary_hello_x64_elf_bin_start;
}

inline uint32_t HelloX64Size() {
    return (uint32_t)((uintptr_t)_binary_hello_x64_elf_bin_end -
                      (uintptr_t)_binary_hello_x64_elf_bin_start);
}

inline bool HasKpython() {
    return KAsset::Has("KPYTHON ELF", _binary_kpython_elf_bin_start, _binary_kpython_elf_bin_end);   // embedded or on-demand from the KURONO partition (satoru)
}
inline const uint8_t* KpythonData() { return KAsset::Data("KPYTHON ELF", _binary_kpython_elf_bin_start, _binary_kpython_elf_bin_end); }   // on-demand asset (satoru)
inline uint32_t KpythonSize() {
    return KAsset::Size("KPYTHON ELF", _binary_kpython_elf_bin_start, _binary_kpython_elf_bin_end);   // embedded or on-demand from the KURONO partition (satoru)
}

// musl-static hello accessors (satoru)
inline bool HasMuslHello() {
    return _binary_hello_musl_elf_start != nullptr &&
           _binary_hello_musl_elf_end   != nullptr &&
           +_binary_hello_musl_elf_end > +_binary_hello_musl_elf_start;
}
inline const uint8_t* MuslHelloData() { return _binary_hello_musl_elf_start; }
inline uint32_t MuslHelloSize() {
    return (uint32_t)((uintptr_t)_binary_hello_musl_elf_end -
                      (uintptr_t)_binary_hello_musl_elf_start);
}

// musl-static ffmpeg accessors (satoru)
inline bool FfmpegLinked() { return KAsset::Linked(_binary_ffmpeg_elf_start, _binary_ffmpeg_elf_end); }   // (satoru)
inline bool KldE1000eLinked() { return KAsset::Linked(_binary_kld_e1000e_elf_start, _binary_kld_e1000e_elf_end); }   // (satoru)
inline bool KpythonLinked() { return KAsset::Linked(_binary_kpython_elf_bin_start, _binary_kpython_elf_bin_end); }   // (satoru)
inline bool HasFfmpeg() {
    return KAsset::Has("FFMPEG  ELF", _binary_ffmpeg_elf_start, _binary_ffmpeg_elf_end);   // embedded or on-demand from the KURONO partition (satoru)
}
inline const uint8_t* FfmpegData() { return KAsset::Data("FFMPEG  ELF", _binary_ffmpeg_elf_start, _binary_ffmpeg_elf_end); }   // on-demand asset (satoru)
inline uint32_t FfmpegSize() {
    return KAsset::Size("FFMPEG  ELF", _binary_ffmpeg_elf_start, _binary_ffmpeg_elf_end);   // embedded or on-demand from the KURONO partition (satoru)
}

// raw-protocol wl_shm wayland test client accessors (satoru)
inline bool HasWlShmTest() {
    return _binary_wl_shm_test_elf_start != nullptr &&
           _binary_wl_shm_test_elf_end   != nullptr &&
           +_binary_wl_shm_test_elf_end > +_binary_wl_shm_test_elf_start;
}
inline const uint8_t* WlShmTestData() { return _binary_wl_shm_test_elf_start; }
inline uint32_t WlShmTestSize() {
    return (uint32_t)((uintptr_t)_binary_wl_shm_test_elf_end -
                      (uintptr_t)_binary_wl_shm_test_elf_start);
}

// L2 mmap hole reuse test accessors (satoru)
inline bool HasMmapReuseTest() {
    return _binary_mmap_reuse_test_elf_start != nullptr &&
           _binary_mmap_reuse_test_elf_end   != nullptr &&
           +_binary_mmap_reuse_test_elf_end > +_binary_mmap_reuse_test_elf_start;
}
inline const uint8_t* MmapReuseTestData() { return _binary_mmap_reuse_test_elf_start; }
inline uint32_t MmapReuseTestSize() {
    return (uint32_t)((uintptr_t)_binary_mmap_reuse_test_elf_end -
                      (uintptr_t)_binary_mmap_reuse_test_elf_start);
}

// pthreads smoke-test accessors (satoru)
inline bool HasPthreadTest() {
    return _binary_pthread_test_elf_start != nullptr &&
           _binary_pthread_test_elf_end   != nullptr &&
           +_binary_pthread_test_elf_end > +_binary_pthread_test_elf_start;
}
inline const uint8_t* PthreadTestData() { return _binary_pthread_test_elf_start; }
inline uint32_t PthreadTestSize() {
    return (uint32_t)((uintptr_t)_binary_pthread_test_elf_end -
                      (uintptr_t)_binary_pthread_test_elf_start);
}

// signal-delivery self-test accessors (satoru)
inline bool HasSignalTest() {
    return _binary_signal_test_elf_start != nullptr &&
           _binary_signal_test_elf_end   != nullptr &&
           +_binary_signal_test_elf_end > +_binary_signal_test_elf_start;
}
inline const uint8_t* SignalTestData() { return _binary_signal_test_elf_start; }
inline uint32_t SignalTestSize() {
    return (uint32_t)((uintptr_t)_binary_signal_test_elf_end -
                      (uintptr_t)_binary_signal_test_elf_start);
}

// interruptible-sleep self-test accessors (satoru)
inline bool HasSleepTest() {
    return _binary_sleeptest_elf_start != nullptr &&
           _binary_sleeptest_elf_end   != nullptr &&
           +_binary_sleeptest_elf_end > +_binary_sleeptest_elf_start;
}
inline const uint8_t* SleepTestData() { return _binary_sleeptest_elf_start; }
inline uint32_t SleepTestSize() {
    return (uint32_t)((uintptr_t)_binary_sleeptest_elf_end -
                      (uintptr_t)_binary_sleeptest_elf_start);
}

// fdtest accessors (satoru)
inline bool HasFdTest() {
    return _binary_fdtest_elf_start != nullptr &&
           _binary_fdtest_elf_end   != nullptr &&
           +_binary_fdtest_elf_end > +_binary_fdtest_elf_start;
}
inline const uint8_t* FdTestData() { return _binary_fdtest_elf_start; }
inline uint32_t FdTestSize() {
    return (uint32_t)((uintptr_t)_binary_fdtest_elf_end -
                      (uintptr_t)_binary_fdtest_elf_start);
}

// kld acpica container accessors (kurono.kld.acpi=1) (satoru)
inline bool HasKldAcpi() {
    return _binary_kld_acpi_elf_start != nullptr && _binary_kld_acpi_elf_end != nullptr &&
           +_binary_kld_acpi_elf_end > +_binary_kld_acpi_elf_start;
}
inline const uint8_t* KldAcpiData() { return _binary_kld_acpi_elf_start; }
inline uint32_t KldAcpiSize() {
    return (uint32_t)((uintptr_t)_binary_kld_acpi_elf_end - (uintptr_t)_binary_kld_acpi_elf_start);
}

// kld real touchpad family accessors (kurono.kld=i2chid) (satoru)
inline bool HasKldI2chid() {
    return _binary_kld_i2chid_elf_start != nullptr && _binary_kld_i2chid_elf_end != nullptr &&
           +_binary_kld_i2chid_elf_end > +_binary_kld_i2chid_elf_start;
}
inline const uint8_t* KldI2chidData() { return _binary_kld_i2chid_elf_start; }
inline uint32_t KldI2chidSize() {
    return (uint32_t)((uintptr_t)_binary_kld_i2chid_elf_end - (uintptr_t)_binary_kld_i2chid_elf_start);
}

// kld network a/b instrument accessors (kurono.kld.bench=1) (satoru)
inline bool HasKldBench() {
    return _binary_kldnetbench_elf_start != nullptr && _binary_kldnetbench_elf_end != nullptr &&
           +_binary_kldnetbench_elf_end > +_binary_kldnetbench_elf_start;
}
inline const uint8_t* KldBenchData() { return _binary_kldnetbench_elf_start; }
inline uint32_t KldBenchSize() {
    return (uint32_t)((uintptr_t)_binary_kldnetbench_elf_end - (uintptr_t)_binary_kldnetbench_elf_start);
}

// userspace wake-latency probe accessors (kurono.irqlat) (satoru)
inline bool HasIrqLat() {
    return _binary_irqlat_elf_start != nullptr &&
           _binary_irqlat_elf_end   != nullptr &&
           +_binary_irqlat_elf_end > +_binary_irqlat_elf_start;
}
inline const uint8_t* IrqLatData() { return _binary_irqlat_elf_start; }
inline uint32_t IrqLatSize() {
    return (uint32_t)((uintptr_t)_binary_irqlat_elf_end -
                      (uintptr_t)_binary_irqlat_elf_start);
}

// cpu scaling probe accessors (kurono.smpscale) (satoru)
inline bool HasSmpScale() {
    return _binary_smpscale_elf_start != nullptr &&
           _binary_smpscale_elf_end   != nullptr &&
           +_binary_smpscale_elf_end > +_binary_smpscale_elf_start;
}
inline const uint8_t* SmpScaleData() { return _binary_smpscale_elf_start; }
inline uint32_t SmpScaleSize() {
    return (uint32_t)((uintptr_t)_binary_smpscale_elf_end - (uintptr_t)_binary_smpscale_elf_start);
}

// posix_spawn static-elf64 repro accessors (kurono.spawntest=1) (satoru)
inline bool HasSpawnTest() {
    return _binary_spawntest_elf_start != nullptr &&
           _binary_spawntest_elf_end   != nullptr &&
           +_binary_spawntest_elf_end > +_binary_spawntest_elf_start;
}
inline const uint8_t* SpawnTestData() { return _binary_spawntest_elf_start; }
inline uint32_t SpawnTestSize() {
    return (uint32_t)((uintptr_t)_binary_spawntest_elf_end -
                      (uintptr_t)_binary_spawntest_elf_start);
}

// KLD driver-container accessors (kurono.kld=1) (satoru)
inline bool HasKldE1000e() {
    return KAsset::Has("KLDE1K  ELF", _binary_kld_e1000e_elf_start, _binary_kld_e1000e_elf_end);   // embedded or on-demand from the KURONO partition (satoru)
}
inline const uint8_t* KldE1000eData() { return KAsset::Data("KLDE1K  ELF", _binary_kld_e1000e_elf_start, _binary_kld_e1000e_elf_end); }   // on-demand asset (satoru)
inline uint32_t KldE1000eSize() {
    return KAsset::Size("KLDE1K  ELF", _binary_kld_e1000e_elf_start, _binary_kld_e1000e_elf_end);   // embedded or on-demand from the KURONO partition (satoru)
}

// KLD shim self-test accessors (kurono.kld=apitest) (satoru)
inline bool HasKldApitest() {
    return _binary_kld_apitest_elf_start != nullptr &&
           _binary_kld_apitest_elf_end   != nullptr &&
           +_binary_kld_apitest_elf_end > +_binary_kld_apitest_elf_start;
}
inline const uint8_t* KldApitestData() { return _binary_kld_apitest_elf_start; }
inline uint32_t KldApitestSize() {
    return (uint32_t)((uintptr_t)_binary_kld_apitest_elf_end -
                      (uintptr_t)_binary_kld_apitest_elf_start);
}

// iwlwifi hardware-container accessors (kurono.kld=iwlwifi) (satoru)
inline bool HasKldIwlwifi() {
    return _binary_kld_iwlwifi_elf_start != nullptr &&
           _binary_kld_iwlwifi_elf_end   != nullptr &&
           +_binary_kld_iwlwifi_elf_end > +_binary_kld_iwlwifi_elf_start;
}
inline const uint8_t* KldIwlwifiData() { return _binary_kld_iwlwifi_elf_start; }
inline uint32_t KldIwlwifiSize() {
    return (uint32_t)((uintptr_t)_binary_kld_iwlwifi_elf_end -
                      (uintptr_t)_binary_kld_iwlwifi_elf_start);
}

// user-pointer validation test accessors (kurono.uatest=1) (satoru)
inline bool HasKenTest() {
    return _binary_kentest_elf_start != nullptr &&
           _binary_kentest_elf_end   != nullptr &&
           +_binary_kentest_elf_end > +_binary_kentest_elf_start;
}
inline const uint8_t* KenTestData() { return _binary_kentest_elf_start; }
inline uint32_t KenTestSize() {
    return (uint32_t)((uintptr_t)_binary_kentest_elf_end -
                      (uintptr_t)_binary_kentest_elf_start);
}

inline bool HasUaTest() {
    return _binary_uatest_elf_start != nullptr &&
           _binary_uatest_elf_end   != nullptr &&
           +_binary_uatest_elf_end > +_binary_uatest_elf_start;
}
inline const uint8_t* UaTestData() { return _binary_uatest_elf_start; }
inline uint32_t UaTestSize() {
    return (uint32_t)((uintptr_t)_binary_uatest_elf_end -
                      (uintptr_t)_binary_uatest_elf_start);
}


// ring-3 audio daemon (kurono.r3audio=1) (satoru)
inline bool HasAudiod() {
    return _binary_audiod_elf_start != nullptr &&
           _binary_audiod_elf_end   != nullptr &&
           +_binary_audiod_elf_end > +_binary_audiod_elf_start;
}
inline const uint8_t* AudiodData() { return _binary_audiod_elf_start; }
inline uint32_t AudiodSize() {
    return (uint32_t)((uintptr_t)_binary_audiod_elf_end -
                      (uintptr_t)_binary_audiod_elf_start);
}

// audio control tool accessors (satoru)
inline bool HasKaudio() {
    return _binary_kaudio_elf_start != nullptr &&
           _binary_kaudio_elf_end   != nullptr &&
           +_binary_kaudio_elf_end > +_binary_kaudio_elf_start;
}
inline const uint8_t* KaudioData() { return _binary_kaudio_elf_start; }
inline uint32_t KaudioSize() {
    return (uint32_t)((uintptr_t)_binary_kaudio_elf_end -
                      (uintptr_t)_binary_kaudio_elf_start);
}

// /dev/kshell gate accessors (kurono.kshtest=1) (satoru)
inline bool HasKshTest() {
    return _binary_kshtest_elf_start != nullptr &&
           _binary_kshtest_elf_end   != nullptr &&
           +_binary_kshtest_elf_end > +_binary_kshtest_elf_start;
}
inline const uint8_t* KshTestData() { return _binary_kshtest_elf_start; }
inline uint32_t KshTestSize() {
    return (uint32_t)((uintptr_t)_binary_kshtest_elf_end -
                      (uintptr_t)_binary_kshtest_elf_start);
}

// busybox accessors (kurono.bbtest=1) (satoru)
inline bool HasBusybox() {
    return _binary_busybox_elf_start != nullptr &&
           _binary_busybox_elf_end   != nullptr &&
           +_binary_busybox_elf_end > +_binary_busybox_elf_start;
}
inline const uint8_t* BusyboxData() { return _binary_busybox_elf_start; }
inline uint32_t BusyboxSize() {
    return (uint32_t)((uintptr_t)_binary_busybox_elf_end -
                      (uintptr_t)_binary_busybox_elf_start);
}

// real bash accessors (optional blob). (satoru)
inline bool HasBashStatic() {
    return KAsset::Has("BASH    ELF", _binary_bash_elf_start, _binary_bash_elf_end);
}
inline const uint8_t* BashStaticData() {
    return KAsset::Data("BASH    ELF", _binary_bash_elf_start, _binary_bash_elf_end);
}
inline uint32_t BashStaticSize() {
    return KAsset::Size("BASH    ELF", _binary_bash_elf_start, _binary_bash_elf_end);
}

// static git accessors (optional blob). (satoru)
inline bool HasGitStatic() {
    return KAsset::Has("GIT     ELF", _binary_git_elf_start, _binary_git_elf_end);
}
inline const uint8_t* GitStaticData() {
    return KAsset::Data("GIT     ELF", _binary_git_elf_start, _binary_git_elf_end);
}
inline uint32_t GitStaticSize() {
    return KAsset::Size("GIT     ELF", _binary_git_elf_start, _binary_git_elf_end);
}

// ksh-run accessors - the agent's bash -> /dev/kshell bridge. (satoru)
inline bool HasKshrun() {
    return _binary_kshrun_elf_start != nullptr &&
           _binary_kshrun_elf_end   != nullptr &&
           +_binary_kshrun_elf_end > +_binary_kshrun_elf_start;
}
inline const uint8_t* KshrunData() { return _binary_kshrun_elf_start; }
inline uint32_t KshrunSize() {
    return (uint32_t)((uintptr_t)_binary_kshrun_elf_end -
                      (uintptr_t)_binary_kshrun_elf_start);
}

// ai recovery client accessors (kurono.dsai=1) (satoru)
inline bool HasDsai() {
    return _binary_dsai_elf_start != nullptr &&
           _binary_dsai_elf_end   != nullptr &&
           +_binary_dsai_elf_end > +_binary_dsai_elf_start;
}
inline bool HasKfetch() {
    return _binary_kfetch_elf_start != nullptr &&
           _binary_kfetch_elf_end   != nullptr &&
           +_binary_kfetch_elf_end > +_binary_kfetch_elf_start;
}
inline const uint8_t* KfetchData() { return _binary_kfetch_elf_start; }
inline uint32_t KfetchSize() {
    return (uint32_t)((uintptr_t)_binary_kfetch_elf_end -
                      (uintptr_t)_binary_kfetch_elf_start);
}
inline const uint8_t* DsaiData() { return _binary_dsai_elf_start; }
inline uint32_t DsaiSize() {
    return (uint32_t)((uintptr_t)_binary_dsai_elf_end -
                      (uintptr_t)_binary_dsai_elf_start);
}
// the api key blob is absent from a default build - HasDsaiKey() is the
// only thing that should ever gate reading it. (satoru)
inline bool HasDsaiKey() {
    return _binary_dsai_key_bin_start != nullptr &&
           _binary_dsai_key_bin_end   != nullptr &&
           +_binary_dsai_key_bin_end > +_binary_dsai_key_bin_start;
}
inline const uint8_t* DsaiKeyData() { return _binary_dsai_key_bin_start; }
inline uint32_t DsaiKeySize() {
    return (uint32_t)((uintptr_t)_binary_dsai_key_bin_end -
                      (uintptr_t)_binary_dsai_key_bin_start);
}

// recovery toolbox accessors - absent unless the tar was staged at build (satoru)
inline bool HasRecoveryTools() {
    return KAsset::Has("RTOOLS  TAR", _binary_recovery_tools_tar_start, _binary_recovery_tools_tar_end);   // embedded or on-demand from the KURONO partition (satoru)
}
inline const uint8_t* RecoveryToolsData() { return KAsset::Data("RTOOLS  TAR", _binary_recovery_tools_tar_start, _binary_recovery_tools_tar_end); }   // on-demand asset (satoru)
inline uint32_t RecoveryToolsSize() {
    return KAsset::Size("RTOOLS  TAR", _binary_recovery_tools_tar_start, _binary_recovery_tools_tar_end);   // embedded or on-demand from the KURONO partition (satoru)
}

// parallel tree-walk test accessors (kurono.findtest=1) (satoru)
inline bool HasFindtest() {
    return _binary_findtest_elf_start != nullptr &&
           _binary_findtest_elf_end   != nullptr &&
           +_binary_findtest_elf_end > +_binary_findtest_elf_start;
}
inline const uint8_t* FindtestData() { return _binary_findtest_elf_start; }
inline uint32_t FindtestSize() {
    return (uint32_t)((uintptr_t)_binary_findtest_elf_end -
                      (uintptr_t)_binary_findtest_elf_start);
}

// dynamic musl pie accessors (satoru)
inline bool HasDyntest() {
    return _binary_dyntest_elf_start != nullptr &&
           _binary_dyntest_elf_end   != nullptr &&
           +_binary_dyntest_elf_end > +_binary_dyntest_elf_start;
}
inline const uint8_t* DyntestData() { return _binary_dyntest_elf_start; }
inline uint32_t DyntestSize() {
    return (uint32_t)((uintptr_t)_binary_dyntest_elf_end -
                      (uintptr_t)_binary_dyntest_elf_start);
}

// musl libc.so accessors (satoru)
inline bool HasMuslLibc() {
    return KAsset::Has("MUSLLIBC SO ", _binary_musl_libc_so_start, _binary_musl_libc_so_end);
}
inline const uint8_t* MuslLibcData() {
    return KAsset::Data("MUSLLIBC SO ", _binary_musl_libc_so_start, _binary_musl_libc_so_end);
}
inline uint32_t MuslLibcSize() {
    return KAsset::Size("MUSLLIBC SO ", _binary_musl_libc_so_start, _binary_musl_libc_so_end);
}

// libfoo.so accessors (satoru)
inline bool HasLibfoo() {
    return _binary_libfoo_so_start != nullptr &&
           _binary_libfoo_so_end   != nullptr &&
           +_binary_libfoo_so_end > +_binary_libfoo_so_start;
}
inline const uint8_t* LibfooData() { return _binary_libfoo_so_start; }
inline uint32_t LibfooSize() {
    return (uint32_t)((uintptr_t)_binary_libfoo_so_end -
                      (uintptr_t)_binary_libfoo_so_start);
}

} // namespace EmbeddedUserprogs

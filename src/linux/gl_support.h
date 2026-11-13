#ifndef KURONO_GL_SUPPORT_H
#define KURONO_GL_SUPPORT_H

// native software gl support: install the alpine mesa tree from a root tar on
// the kurono partition, then run the unmodified distro binaries through the
// same ld-kurono / real-musl path firefox uses. dev harness for job gl (satoru)

struct KuronoShell;

namespace GlSupport {

// glsetup: read GLTAR.TAR from the KURONO fat32 root, unpack it to kvfs, make
// the soname symlinks the tarball cannot carry. (satoru)
int CmdSetup(KuronoShell* sh, int argc, const char** argv, char* out, int mx);

// glrun [KEY=VALUE...] <program> [args...]: run one elfin a wayland/egl test
// environment. leading KEY=VALUE words override the default environment. (satoru)
int CmdRun(KuronoShell* sh, int argc, const char** argv, char* out, int mx);

}  // namespace GlSupport

#endif
// end (satoru)

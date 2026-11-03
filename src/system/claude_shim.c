// bash shim: claude code insists on a binary named bash/zsh; kurono ships busybox sh. (satoru)
extern char **environ;
int execve(const char*, char *const[], char *const[]);
int main(int argc, char **argv) {
    (void)argc;
    argv[0] = "sh";
    execve("/system/bin/busybox", argv, environ);
    return 127;
}
// end (satoru)

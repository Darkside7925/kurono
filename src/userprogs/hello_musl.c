// tiny musl-static hello used to prove the real elf loader path (satoru)
#include <stdio.h>

int main(void) {
    printf("hello from static elf\n");
    return 0;
}

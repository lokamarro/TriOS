// An actual ARM64 guest BRK must terminate only this Linux process.
int main(void) {
#if defined(__aarch64__)
    __asm__ volatile("brk #0" ::: "memory");
#endif
    return 2;
}

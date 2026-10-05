static long
kernel(long n, long a1, long a2, long a3) {
  long ret;
  long iserror;
  register long r10 __asm__("r10")=a3;

  __asm__ __volatile__("syscall"
		       : "=a"(ret), "=@ccc"(iserror)
		       : "a"(n), "D"(a1), "S"(a2), "r"(r10)
		       : "rcx", "r11", "memory");

  return iserror ? -ret : ret;
}

void
_start(void) {
  kernel(1, 0, 0, 0);
  __builtin_trap();
}

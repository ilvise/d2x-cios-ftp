/*
 * Host test stub of cios-lib/syscalls.h: cache maintenance is not needed
 * on the PC, the IOS socket layer is replaced by host_shim.c.
 */
#define os_sync_before_read(p, s) do {} while (0)
#define os_sync_after_write(p, s) do {} while (0)

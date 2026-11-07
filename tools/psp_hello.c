// tools/psp_hello.c — minimal PSP boot probe.
// Writes ms0:/HELLO.txt and exits. Used to verify PPSSPP actually runs our EBOOT.
#include <pspkernel.h>
#include <pspiofilemgr.h>

PSP_MODULE_INFO("PLHello", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(PSP_THREAD_ATTR_USER);
PSP_MAIN_THREAD_STACK_SIZE_KB(32);

int main(void) {
  const SceUID fd = sceIoOpen("ms0:/HELLO.txt", PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
  if (fd >= 0) {
    sceIoWrite(fd, "hello\n", 6);
    sceIoClose(fd);
  }
  sceKernelExitGame();
  return 0;
}

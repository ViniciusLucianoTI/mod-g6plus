/*
 * g6reboot: leave iOS on the Moto G6 Plus from a shell (sync, then reboot(2)). The phone resets and its own
 * bootloader starts Android again: the way out of a RAM boot without holding the power key or waiting for the
 * kernel's self-reset timer. Build: tools/g6plus/build-tools.sh; lives in /usr/local/bin on the iOS root.
 */
#include <stdio.h>
#include <unistd.h>

/* not in the iOS SDK's headers */
extern int reboot(int howto);
#define RB_AUTOBOOT 0

int
main(void)
{
	sync();
	sleep(1);
	if (reboot(RB_AUTOBOOT) != 0) {
		perror("reboot");
		return 1;
	}
	return 0;
}

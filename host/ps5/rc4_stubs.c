/* (PS5) Longshot rc.4's crash window and restart, answered on the console (ps5/build.py leaves out host/hos_crash_dialog.c and
   host/hos_relaunch.c). The crash window is a second process the game starts to wait for its end (posix_spawn), and a restart
   the game made again by execve of itself: a title on the console can do neither. A crash still writes its record and the
   log; klog has the rest. */
#include <stdlib.h>
#include "hos.h"

int hos_crash_helper_main(int argc, char **argv) { (void)argc, (void)argv; return -1; }   /* (never the helper) */
void hos_crash_watch_start(int argc, char **argv) { (void)argc, (void)argv; }
void hos_crash_test_arm(void) {}
void hos_relaunch_ask(void) {}       /* (the Insert menu's Restart now: nothing; the title is closed and started from the home screen) */
void hos_relaunch_keep_env(void) {}
void hos_window_closed_exit(void) {
    void hos_crash_clean_end(void);
    hos_crash_clean_end();   /* (the run's marker removed: a quit, not an unclean end) */
    void hos_ps5_exit(int code);
    hos_ps5_exit(0);
}

/* The game code the console's build keeps from before rc.4 (ps5/build.py does not lift again, so its objects stay): its lift
   still reads hos_occlusion_off at six sites (nx2/oot_occlusion.py, Fritchii's Occlusion Off), which rc.4 removed with its
   switch. 0: the game makes its occlusion queries, as rc.4 has it */
volatile int hos_occlusion_off;

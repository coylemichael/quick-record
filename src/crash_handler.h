#ifndef CRASH_HANDLER_H
#define CRASH_HANDLER_H

/*
 * Comprehensive Crash Handler API
 * 
 * Catches: SEH exceptions, CRT errors, signals, stack overflow, heap corruption, hangs
 */

// Initialize all crash handlers - call as early as possible in main()
void CrashHandler_Init(void);

// Clean shutdown - removes handlers, frees resources
void CrashHandler_Shutdown(void);

// Watchdog for hang detection - optional
// Call CrashHandler_StartWatchdog() when entering main processing loop
// Call CrashHandler_Heartbeat() periodically from main thread (e.g., in message loop)
// If no heartbeat for 30 seconds, a hang is detected and crash dump is created
void CrashHandler_StartWatchdog(void);
void CrashHandler_StopWatchdog(void);
void CrashHandler_Heartbeat(void);

// Force crash for testing (triggers access violation)
void CrashHandler_ForceCrash(void);

#endif // CRASH_HANDLER_H

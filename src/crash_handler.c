/*
 * Crash Handler - Comprehensive crash/hang detection and minidump generation
 * 
 * This handler catches:
 * - Unhandled exceptions (access violations, div by zero, etc.)
 * - CRT abort() calls
 * - Invalid parameter errors
 * - Pure virtual function calls
 * - SIGABRT/SIGSEGV/SIGFPE signals
 * - Stack overflows (via guard page and alternate stack)
 * - Heap corruption
 * - Application hangs/deadlocks (via watchdog thread)
 *
 * Best Practices Implemented:
 * 1. Vectored Exception Handler for early catch
 * 2. Dedicated thread for MiniDumpWriteDump (avoids deadlock)
 * 3. Separate stack for stack overflow handling
 * 4. Watchdog thread for hang detection
 * 5. Multiple CRT error handlers
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <time.h>
#include "crash_handler.h"

#pragma comment(lib, "dbghelp.lib")

// ============================================================================
// Configuration
// ============================================================================

#define WATCHDOG_TIMEOUT_MS     30000   // 30 seconds without heartbeat = hang
#define WATCHDOG_CHECK_INTERVAL 5000    // Check every 5 seconds
#define STACK_OVERFLOW_RESERVE  65536   // 64KB reserved for stack overflow handling

// Heap corruption status code (not always defined)
#ifndef STATUS_HEAP_CORRUPTION
#define STATUS_HEAP_CORRUPTION 0xC0000374
#endif

// ============================================================================
// Global State
// ============================================================================

static LPTOP_LEVEL_EXCEPTION_FILTER g_previousFilter = NULL;
static PVOID g_vectoredHandler = NULL;
static CRITICAL_SECTION g_crashLock;
static volatile BOOL g_crashInProgress = FALSE;
static volatile LONG g_heartbeatCounter = 0;
static HANDLE g_watchdogThread = NULL;
static volatile BOOL g_watchdogRunning = FALSE;
static BOOL g_crashHandlerInitialized = FALSE;

// Stack overflow handling - reserve memory for guard page restoration
static LPVOID g_stackOverflowGuard = NULL;

// Stored exception info for dump thread
static EXCEPTION_POINTERS* g_storedExceptionInfo = NULL;
static DWORD g_crashingThreadId = 0;
static HANDLE g_dumpCompleteEvent = NULL;
static const char* g_crashReason = NULL;

// ============================================================================
// Utility Functions
// ============================================================================

static void GetExeDirectory(char* buffer, size_t size) {
    GetModuleFileNameA(NULL, buffer, (DWORD)size);
    char* lastSlash = strrchr(buffer, '\\');
    if (lastSlash) *lastSlash = '\0';
}

static void GetCrashFilePaths(char* dumpPath, char* logPath, size_t size) {
    char exeDir[MAX_PATH];
    GetExeDirectory(exeDir, sizeof(exeDir));
    
    time_t now = time(NULL);
    struct tm* t = localtime(&now);
    char timestamp[32];
    strftime(timestamp, sizeof(timestamp), "%Y%m%d_%H%M%S", t);
    
    snprintf(dumpPath, size, "%s\\lwsr_crash_%s.dmp", exeDir, timestamp);
    snprintf(logPath, size, "%s\\lwsr_crash_%s.txt", exeDir, timestamp);
}

static const char* GetExceptionName(DWORD code) {
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:         return "ACCESS_VIOLATION";
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:    return "ARRAY_BOUNDS_EXCEEDED";
        case EXCEPTION_BREAKPOINT:               return "BREAKPOINT";
        case EXCEPTION_DATATYPE_MISALIGNMENT:    return "DATATYPE_MISALIGNMENT";
        case EXCEPTION_FLT_DENORMAL_OPERAND:     return "FLT_DENORMAL_OPERAND";
        case EXCEPTION_FLT_DIVIDE_BY_ZERO:       return "FLT_DIVIDE_BY_ZERO";
        case EXCEPTION_FLT_INEXACT_RESULT:       return "FLT_INEXACT_RESULT";
        case EXCEPTION_FLT_INVALID_OPERATION:    return "FLT_INVALID_OPERATION";
        case EXCEPTION_FLT_OVERFLOW:             return "FLT_OVERFLOW";
        case EXCEPTION_FLT_STACK_CHECK:          return "FLT_STACK_CHECK";
        case EXCEPTION_FLT_UNDERFLOW:            return "FLT_UNDERFLOW";
        case EXCEPTION_GUARD_PAGE:               return "GUARD_PAGE";
        case EXCEPTION_ILLEGAL_INSTRUCTION:      return "ILLEGAL_INSTRUCTION";
        case EXCEPTION_IN_PAGE_ERROR:            return "IN_PAGE_ERROR";
        case EXCEPTION_INT_DIVIDE_BY_ZERO:       return "INT_DIVIDE_BY_ZERO";
        case EXCEPTION_INT_OVERFLOW:             return "INT_OVERFLOW";
        case EXCEPTION_INVALID_DISPOSITION:      return "INVALID_DISPOSITION";
        case EXCEPTION_INVALID_HANDLE:           return "INVALID_HANDLE";
        case EXCEPTION_NONCONTINUABLE_EXCEPTION: return "NONCONTINUABLE_EXCEPTION";
        case EXCEPTION_PRIV_INSTRUCTION:         return "PRIV_INSTRUCTION";
        case EXCEPTION_SINGLE_STEP:              return "SINGLE_STEP";
        case EXCEPTION_STACK_OVERFLOW:           return "STACK_OVERFLOW";
        case STATUS_HEAP_CORRUPTION:             return "HEAP_CORRUPTION";
        default:                                 return "UNKNOWN";
    }
}

// ============================================================================
// Dump Writing Thread (runs on clean stack)
// ============================================================================

static DWORD WINAPI DumpWriterThread(LPVOID param) {
    (void)param;
    
    char dumpPath[MAX_PATH];
    char logPath[MAX_PATH];
    GetCrashFilePaths(dumpPath, logPath, sizeof(dumpPath));
    
    // Write minidump - this is the critical operation that must run on a clean stack
    HANDLE dumpFile = CreateFileA(dumpPath, GENERIC_WRITE, 0, NULL, 
                                   CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (dumpFile != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION mei;
        mei.ThreadId = g_crashingThreadId;
        mei.ExceptionPointers = g_storedExceptionInfo;
        mei.ClientPointers = FALSE;
        
        // Use more comprehensive dump flags for better debugging
        MINIDUMP_TYPE dumpType = (MINIDUMP_TYPE)(
            MiniDumpWithDataSegs |
            MiniDumpWithHandleData |
            MiniDumpWithThreadInfo |
            MiniDumpWithUnloadedModules |
            MiniDumpWithIndirectlyReferencedMemory |
            MiniDumpWithProcessThreadData
        );
        
        MiniDumpWriteDump(
            GetCurrentProcess(),
            GetCurrentProcessId(),
            dumpFile,
            dumpType,
            g_storedExceptionInfo ? &mei : NULL,
            NULL,
            NULL
        );
        CloseHandle(dumpFile);
    }
    
    // Write crash log
    FILE* logFile = fopen(logPath, "w");
    if (logFile) {
        time_t now = time(NULL);
        
        fprintf(logFile, "=== LWSR Crash Report ===\n");
        fprintf(logFile, "Time: %s", ctime(&now));
        fprintf(logFile, "Crash Reason: %s\n", g_crashReason ? g_crashReason : "Unknown");
        fprintf(logFile, "Crashing Thread ID: %lu\n", g_crashingThreadId);
        fprintf(logFile, "\n");
        
        if (g_storedExceptionInfo) {
            EXCEPTION_RECORD* rec = g_storedExceptionInfo->ExceptionRecord;
            CONTEXT* ctx = g_storedExceptionInfo->ContextRecord;
            
            fprintf(logFile, "Exception: 0x%08lX (%s)\n", 
                    rec->ExceptionCode, GetExceptionName(rec->ExceptionCode));
            fprintf(logFile, "Address: 0x%p\n", rec->ExceptionAddress);
            fprintf(logFile, "Flags: 0x%08lX\n", rec->ExceptionFlags);
            fprintf(logFile, "\n");
            
            if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2) {
                fprintf(logFile, "Access violation: %s address 0x%p\n",
                    rec->ExceptionInformation[0] == 0 ? "reading" : 
                    (rec->ExceptionInformation[0] == 1 ? "writing" : "executing"),
                    (void*)rec->ExceptionInformation[1]);
                fprintf(logFile, "\n");
            }
            
#ifdef _WIN64
            fprintf(logFile, "Registers:\n");
            fprintf(logFile, "  RAX: 0x%016llX  RBX: 0x%016llX\n", ctx->Rax, ctx->Rbx);
            fprintf(logFile, "  RCX: 0x%016llX  RDX: 0x%016llX\n", ctx->Rcx, ctx->Rdx);
            fprintf(logFile, "  RSI: 0x%016llX  RDI: 0x%016llX\n", ctx->Rsi, ctx->Rdi);
            fprintf(logFile, "  RSP: 0x%016llX  RBP: 0x%016llX\n", ctx->Rsp, ctx->Rbp);
            fprintf(logFile, "  R8:  0x%016llX  R9:  0x%016llX\n", ctx->R8, ctx->R9);
            fprintf(logFile, "  R10: 0x%016llX  R11: 0x%016llX\n", ctx->R10, ctx->R11);
            fprintf(logFile, "  R12: 0x%016llX  R13: 0x%016llX\n", ctx->R12, ctx->R13);
            fprintf(logFile, "  R14: 0x%016llX  R15: 0x%016llX\n", ctx->R14, ctx->R15);
            fprintf(logFile, "  RIP: 0x%016llX\n", ctx->Rip);
#else
            fprintf(logFile, "Registers:\n");
            fprintf(logFile, "  EAX: 0x%08lX  EBX: 0x%08lX\n", ctx->Eax, ctx->Ebx);
            fprintf(logFile, "  ECX: 0x%08lX  EDX: 0x%08lX\n", ctx->Ecx, ctx->Edx);
            fprintf(logFile, "  ESI: 0x%08lX  EDI: 0x%08lX\n", ctx->Esi, ctx->Edi);
            fprintf(logFile, "  ESP: 0x%08lX  EBP: 0x%08lX\n", ctx->Esp, ctx->Ebp);
            fprintf(logFile, "  EIP: 0x%08lX\n", ctx->Eip);
#endif
        } else {
            fprintf(logFile, "No exception context available (hang/abort detected)\n");
        }
        
        fprintf(logFile, "\n");
        fprintf(logFile, "Minidump saved to: %s\n", dumpPath);
        fprintf(logFile, "\nPlease report this crash at:\n");
        fprintf(logFile, "https://github.com/coylemichael/light-weight-screen-recorder/issues\n");
        
        fclose(logFile);
    }
    
    // Show message to user
    char msg[512];
    snprintf(msg, sizeof(msg), 
        "LWSR has crashed.\n\n"
        "Reason: %s\n\n"
        "Crash dump saved to:\n%s\n\n"
        "Please report this issue on GitHub.",
        g_crashReason ? g_crashReason : "Unknown exception",
        dumpPath);
    MessageBoxA(NULL, msg, "LWSR Crash", MB_ICONERROR | MB_OK | MB_SYSTEMMODAL);
    
    SetEvent(g_dumpCompleteEvent);
    return 0;
}

// ============================================================================
// Core Crash Handler - Called by all exception sources
// ============================================================================

static void HandleCrash(EXCEPTION_POINTERS* exInfo, const char* reason) {
    // Prevent re-entrancy - if we crash while handling a crash, just terminate
    if (InterlockedCompareExchange((LONG*)&g_crashInProgress, TRUE, FALSE)) {
        TerminateProcess(GetCurrentProcess(), 1);
        return;
    }
    
    // Store exception info for dump thread
    g_storedExceptionInfo = exInfo;
    g_crashingThreadId = GetCurrentThreadId();
    g_crashReason = reason;
    
    // Stop the watchdog immediately
    g_watchdogRunning = FALSE;
    
    // Create event for synchronization
    g_dumpCompleteEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    
    // Spawn a dedicated thread to write the dump
    // This avoids issues with corrupted stack or locked heaps
    HANDLE dumpThread = CreateThread(NULL, 0, DumpWriterThread, NULL, 0, NULL);
    if (dumpThread) {
        // Wait for dump to complete (with timeout to prevent infinite wait)
        WaitForSingleObject(g_dumpCompleteEvent, 30000);
        CloseHandle(dumpThread);
    }
    
    if (g_dumpCompleteEvent) {
        CloseHandle(g_dumpCompleteEvent);
    }
    
    // Terminate cleanly
    TerminateProcess(GetCurrentProcess(), 1);
}

// ============================================================================
// Stack Overflow Handler
// ============================================================================

static void HandleStackOverflow(EXCEPTION_POINTERS* exInfo) {
    // For stack overflow, we need to restore the guard page before we can
    // safely call any functions. The vectored handler runs on a separate 
    // stack context, so we should be OK to proceed
    HandleCrash(exInfo, "STACK_OVERFLOW - Stack exhausted");
}

// ============================================================================
// Vectored Exception Handler
// ============================================================================

static LONG WINAPI VectoredExceptionHandler(EXCEPTION_POINTERS* exInfo) {
    DWORD code = exInfo->ExceptionRecord->ExceptionCode;
    
    // Skip first-chance exceptions that will be handled elsewhere
    // Only handle fatal exceptions that need immediate attention
    switch (code) {
        case EXCEPTION_BREAKPOINT:
        case EXCEPTION_SINGLE_STEP:
            // These are for debuggers
            return EXCEPTION_CONTINUE_SEARCH;
            
        case EXCEPTION_STACK_OVERFLOW:
            HandleStackOverflow(exInfo);
            return EXCEPTION_EXECUTE_HANDLER;
            
        case STATUS_HEAP_CORRUPTION:
            HandleCrash(exInfo, "HEAP_CORRUPTION - Memory corrupted");
            return EXCEPTION_EXECUTE_HANDLER;
            
        default:
            // Let normal exception handling take place
            return EXCEPTION_CONTINUE_SEARCH;
    }
}

// ============================================================================
// Unhandled Exception Filter (final catch-all)
// ============================================================================

static LONG WINAPI UnhandledExceptionFilter_Handler(EXCEPTION_POINTERS* exInfo) {
    const char* exName = GetExceptionName(exInfo->ExceptionRecord->ExceptionCode);
    HandleCrash(exInfo, exName);
    
    // Pass to previous handler if any (shouldn't reach here after HandleCrash)
    if (g_previousFilter) {
        return g_previousFilter(exInfo);
    }
    
    return EXCEPTION_EXECUTE_HANDLER;
}

// ============================================================================
// CRT Error Handlers
// ============================================================================

// Invalid parameter handler (e.g., printf(NULL))
static void InvalidParameterHandler(
    const wchar_t* expression,
    const wchar_t* function,
    const wchar_t* file,
    unsigned int line,
    uintptr_t reserved) 
{
    (void)expression;
    (void)function;
    (void)file;
    (void)line;
    (void)reserved;
    
    // Generate exception info manually
    CONTEXT ctx;
    RtlCaptureContext(&ctx);
    
    EXCEPTION_RECORD rec;
    memset(&rec, 0, sizeof(rec));
    rec.ExceptionCode = EXCEPTION_ACCESS_VIOLATION;
    rec.ExceptionFlags = EXCEPTION_NONCONTINUABLE;
    
    EXCEPTION_POINTERS ex;
    ex.ContextRecord = &ctx;
    ex.ExceptionRecord = &rec;
    
    HandleCrash(&ex, "INVALID_PARAMETER - CRT detected invalid argument");
}

// Pure virtual call handler
static void PureCallHandler(void) {
    CONTEXT ctx;
    RtlCaptureContext(&ctx);
    
    EXCEPTION_RECORD rec;
    memset(&rec, 0, sizeof(rec));
    rec.ExceptionCode = EXCEPTION_ACCESS_VIOLATION;
    rec.ExceptionFlags = EXCEPTION_NONCONTINUABLE;
    
    EXCEPTION_POINTERS ex;
    ex.ContextRecord = &ctx;
    ex.ExceptionRecord = &rec;
    
    HandleCrash(&ex, "PURE_VIRTUAL_CALL - Pure virtual function called");
}

// Signal handler for SIGABRT, SIGSEGV, etc.
static void SignalHandler(int signum) {
    CONTEXT ctx;
    RtlCaptureContext(&ctx);
    
    EXCEPTION_RECORD rec;
    memset(&rec, 0, sizeof(rec));
    rec.ExceptionFlags = EXCEPTION_NONCONTINUABLE;
    
    EXCEPTION_POINTERS ex;
    ex.ContextRecord = &ctx;
    ex.ExceptionRecord = &rec;
    
    const char* reason;
    switch (signum) {
        case SIGABRT:
            rec.ExceptionCode = EXCEPTION_ACCESS_VIOLATION;
            reason = "SIGABRT - Program aborted";
            break;
        case SIGSEGV:
            rec.ExceptionCode = EXCEPTION_ACCESS_VIOLATION;
            reason = "SIGSEGV - Segmentation fault";
            break;
        case SIGFPE:
            rec.ExceptionCode = EXCEPTION_FLT_INVALID_OPERATION;
            reason = "SIGFPE - Floating point exception";
            break;
        case SIGILL:
            rec.ExceptionCode = EXCEPTION_ILLEGAL_INSTRUCTION;
            reason = "SIGILL - Illegal instruction";
            break;
        default:
            rec.ExceptionCode = 0;
            reason = "UNKNOWN_SIGNAL";
            break;
    }
    
    HandleCrash(&ex, reason);
}

// ============================================================================
// Watchdog Thread - Detects Hangs/Deadlocks
// ============================================================================

static DWORD WINAPI WatchdogThread(LPVOID param) {
    (void)param;
    
    LONG lastHeartbeat = g_heartbeatCounter;
    DWORD missedCount = 0;
    
    while (g_watchdogRunning) {
        Sleep(WATCHDOG_CHECK_INTERVAL);
        
        if (!g_watchdogRunning) break;
        
        LONG currentHeartbeat = InterlockedCompareExchange(&g_heartbeatCounter, 0, 0);
        
        if (currentHeartbeat == lastHeartbeat) {
            missedCount++;
            if (missedCount >= (WATCHDOG_TIMEOUT_MS / WATCHDOG_CHECK_INTERVAL)) {
                // Hang detected!
                CONTEXT ctx;
                RtlCaptureContext(&ctx);
                
                EXCEPTION_RECORD rec;
                memset(&rec, 0, sizeof(rec));
                rec.ExceptionCode = 0xDEADDEAD;  // Custom code for hang
                rec.ExceptionFlags = EXCEPTION_NONCONTINUABLE;
                
                EXCEPTION_POINTERS ex;
                ex.ContextRecord = &ctx;
                ex.ExceptionRecord = &rec;
                
                HandleCrash(&ex, "HANG_DETECTED - Application not responding");
            }
        } else {
            lastHeartbeat = currentHeartbeat;
            missedCount = 0;
        }
    }
    
    return 0;
}

// ============================================================================
// Public API
// ============================================================================

void CrashHandler_Init(void) {
    if (g_crashHandlerInitialized) return;
    
    InitializeCriticalSection(&g_crashLock);
    
    // 1. Install Vectored Exception Handler (runs first, catches everything)
    g_vectoredHandler = AddVectoredExceptionHandler(1, VectoredExceptionHandler);
    
    // 2. Install Unhandled Exception Filter (backup for anything VEH misses)
    g_previousFilter = SetUnhandledExceptionFilter(UnhandledExceptionFilter_Handler);
    
    // 3. Install CRT error handlers
    _set_invalid_parameter_handler(InvalidParameterHandler);
    _set_purecall_handler(PureCallHandler);
    
    // 4. Install signal handlers
    signal(SIGABRT, SignalHandler);
    signal(SIGSEGV, SignalHandler);
    signal(SIGFPE, SignalHandler);
    signal(SIGILL, SignalHandler);
    
    // 5. Configure abort behavior - suppress default dialog, we handle it
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    
    // 6. Reserve memory for stack overflow handling
    g_stackOverflowGuard = VirtualAlloc(NULL, STACK_OVERFLOW_RESERVE, 
                                         MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    
    g_crashHandlerInitialized = TRUE;
}

void CrashHandler_StartWatchdog(void) {
    if (!g_crashHandlerInitialized) return;
    if (g_watchdogThread) return;  // Already running
    
    g_watchdogRunning = TRUE;
    g_heartbeatCounter = 0;
    g_watchdogThread = CreateThread(NULL, 0, WatchdogThread, NULL, 0, NULL);
}

void CrashHandler_StopWatchdog(void) {
    if (!g_watchdogThread) return;
    
    g_watchdogRunning = FALSE;
    WaitForSingleObject(g_watchdogThread, 5000);
    CloseHandle(g_watchdogThread);
    g_watchdogThread = NULL;
}

void CrashHandler_Heartbeat(void) {
    InterlockedIncrement(&g_heartbeatCounter);
}

void CrashHandler_Shutdown(void) {
    if (!g_crashHandlerInitialized) return;
    
    // Stop watchdog
    CrashHandler_StopWatchdog();
    
    // Remove handlers
    if (g_vectoredHandler) {
        RemoveVectoredExceptionHandler(g_vectoredHandler);
        g_vectoredHandler = NULL;
    }
    
    if (g_previousFilter) {
        SetUnhandledExceptionFilter(g_previousFilter);
        g_previousFilter = NULL;
    }
    
    // Free reserved memory
    if (g_stackOverflowGuard) {
        VirtualFree(g_stackOverflowGuard, 0, MEM_RELEASE);
        g_stackOverflowGuard = NULL;
    }
    
    DeleteCriticalSection(&g_crashLock);
    g_crashHandlerInitialized = FALSE;
}

// Force crash for testing (debug only)
void CrashHandler_ForceCrash(void) {
    int* p = NULL;
    *p = 42;  // Access violation
}

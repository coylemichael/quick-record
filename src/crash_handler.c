/*
 * Crash Handler - Creates minidump on unhandled exceptions
 * Writes lwsr_crash.dmp and lwsr_crash.txt to the exe directory
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>
#include <stdio.h>
#include <time.h>
#include "crash_handler.h"

#pragma comment(lib, "dbghelp.lib")

static LPTOP_LEVEL_EXCEPTION_FILTER g_previousFilter = NULL;

static void GetExeDirectory(char* buffer, size_t size) {
    GetModuleFileNameA(NULL, buffer, (DWORD)size);
    char* lastSlash = strrchr(buffer, '\\');
    if (lastSlash) *lastSlash = '\0';
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
        case EXCEPTION_ILLEGAL_INSTRUCTION:      return "ILLEGAL_INSTRUCTION";
        case EXCEPTION_IN_PAGE_ERROR:            return "IN_PAGE_ERROR";
        case EXCEPTION_INT_DIVIDE_BY_ZERO:       return "INT_DIVIDE_BY_ZERO";
        case EXCEPTION_INT_OVERFLOW:             return "INT_OVERFLOW";
        case EXCEPTION_INVALID_DISPOSITION:      return "INVALID_DISPOSITION";
        case EXCEPTION_NONCONTINUABLE_EXCEPTION: return "NONCONTINUABLE_EXCEPTION";
        case EXCEPTION_PRIV_INSTRUCTION:         return "PRIV_INSTRUCTION";
        case EXCEPTION_SINGLE_STEP:              return "SINGLE_STEP";
        case EXCEPTION_STACK_OVERFLOW:           return "STACK_OVERFLOW";
        default:                                 return "UNKNOWN";
    }
}

static LONG WINAPI CrashExceptionFilter(EXCEPTION_POINTERS* exInfo) {
    char exeDir[MAX_PATH];
    char dumpPath[MAX_PATH];
    char logPath[MAX_PATH];
    
    GetExeDirectory(exeDir, sizeof(exeDir));
    
    // Generate timestamp for unique filenames
    time_t now = time(NULL);
    struct tm* t = localtime(&now);
    char timestamp[32];
    strftime(timestamp, sizeof(timestamp), "%Y%m%d_%H%M%S", t);
    
    snprintf(dumpPath, sizeof(dumpPath), "%s\\lwsr_crash_%s.dmp", exeDir, timestamp);
    snprintf(logPath, sizeof(logPath), "%s\\lwsr_crash_%s.txt", exeDir, timestamp);
    
    // Write minidump
    HANDLE dumpFile = CreateFileA(dumpPath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (dumpFile != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION mei;
        mei.ThreadId = GetCurrentThreadId();
        mei.ExceptionPointers = exInfo;
        mei.ClientPointers = FALSE;
        
        MiniDumpWriteDump(
            GetCurrentProcess(),
            GetCurrentProcessId(),
            dumpFile,
            MiniDumpWithDataSegs | MiniDumpWithHandleData | MiniDumpWithThreadInfo,
            &mei,
            NULL,
            NULL
        );
        CloseHandle(dumpFile);
    }
    
    // Write crash log
    FILE* logFile = fopen(logPath, "w");
    if (logFile) {
        EXCEPTION_RECORD* rec = exInfo->ExceptionRecord;
        CONTEXT* ctx = exInfo->ContextRecord;
        
        fprintf(logFile, "=== LWSR Crash Report ===\n");
        fprintf(logFile, "Time: %s", ctime(&now));
        fprintf(logFile, "\n");
        
        fprintf(logFile, "Exception: 0x%08lX (%s)\n", rec->ExceptionCode, GetExceptionName(rec->ExceptionCode));
        fprintf(logFile, "Address: 0x%p\n", rec->ExceptionAddress);
        fprintf(logFile, "Flags: 0x%08lX\n", rec->ExceptionFlags);
        fprintf(logFile, "\n");
        
        if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2) {
            fprintf(logFile, "Access violation: %s address 0x%p\n",
                rec->ExceptionInformation[0] == 0 ? "reading" : "writing",
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
        "Crash dump saved to:\n%s\n\n"
        "Please report this issue on GitHub.",
        dumpPath);
    MessageBoxA(NULL, msg, "LWSR Crash", MB_ICONERROR | MB_OK);
    
    // Pass to previous handler if any
    if (g_previousFilter) {
        return g_previousFilter(exInfo);
    }
    
    return EXCEPTION_EXECUTE_HANDLER;
}

void CrashHandler_Init(void) {
    g_previousFilter = SetUnhandledExceptionFilter(CrashExceptionFilter);
}

/* Amiga structure layouts and constants (public AmigaOS ABI, NDK 3.x).
 * Everything here describes memory as 68k programs see it.
 */
#ifndef AMIGA_AMIGA_H
#define AMIGA_AMIGA_H

/* exec/nodes.h */
#define LN_SUCC 0
#define LN_PRED 4
#define LN_TYPE 8
#define LN_PRI 9
#define LN_NAME 10
#define LN_SIZE 14

enum { NT_UNKNOWN, NT_TASK, NT_INTERRUPT, NT_DEVICE, NT_MSGPORT, NT_MESSAGE, NT_FREEMSG,
       NT_REPLYMSG, NT_RESOURCE, NT_LIBRARY, NT_MEMORY, NT_SOFTINT, NT_FONT, NT_PROCESS,
       NT_SEMAPHORE, NT_SIGNALSEM, NT_BOOTNODE, NT_KICKMEM, NT_GRAPHICS, NT_DEATHMESSAGE };

/* exec/lists.h */
#define LH_HEAD 0
#define LH_TAIL 4
#define LH_TAILPRED 8
#define LH_TYPE 12
#define LH_SIZE 14
#define MLH_SIZE 12

/* exec/libraries.h */
#define LIB_FLAGS 14
#define LIB_NEGSIZE 16
#define LIB_POSSIZE 18
#define LIB_VERSION 20
#define LIB_REVISION 22
#define LIB_IDSTRING 24
#define LIB_SUM 28
#define LIB_OPENCNT 32
#define LIB_SIZE 34

/* exec/execbase.h */
#define EB_SOFTVER 34
#define EB_THISTASK 276
#define EB_IDNESTCNT 294
#define EB_TDNESTCNT 295
#define EB_ATTNFLAGS 296
#define EB_MEMLIST 322
#define EB_RESOURCELIST 336
#define EB_DEVICELIST 350
#define EB_INTRLIST 364
#define EB_LIBLIST 378
#define EB_PORTLIST 392
#define EB_TASKREADY 406
#define EB_TASKWAIT 420
#define EB_VBLANKFREQ 530
#define EB_POWERFREQ 531
#define EB_SEMAPHORELIST 532
#define EB_ECLOCKFREQ 568
#define EB_SIZE 632
#define AFF_68010 0x01
#define AFF_68020 0x02
#define AFF_68881 0x10
#define AFF_68882 0x20

/* exec/tasks.h */
#define TC_FLAGS 14
#define TC_STATE 15
#define TC_IDNESTCNT 16
#define TC_TDNESTCNT 17
#define TC_SIGALLOC 18
#define TC_SIGWAIT 22
#define TC_SIGRECVD 26
#define TC_SIGEXCEPT 30
#define TC_TRAPALLOC 34
#define TC_TRAPABLE 36
#define TC_EXCEPTDATA 38
#define TC_EXCEPTCODE 42
#define TC_TRAPDATA 46
#define TC_TRAPCODE 50
#define TC_SPREG 54
#define TC_SPLOWER 58
#define TC_SPUPPER 62
#define TC_SWITCH 66
#define TC_LAUNCH 70
#define TC_MEMENTRY 74
#define TC_USERDATA 88
#define TC_SIZE 92
enum { TS_INVALID, TS_ADDED, TS_RUN, TS_READY, TS_WAIT, TS_EXCEPT, TS_REMOVED };
#define SIGF_ABORT 0x01
#define SIGF_CHILD 0x02
#define SIGF_BLIT 0x10
#define SIGF_SINGLE 0x10
#define SIGF_INTUITION 0x20
#define SIGF_NET 0x80
#define SIGF_DOS 0x100
#define SIGBREAKF_CTRL_C 0x1000
#define SIGBREAKF_CTRL_D 0x2000
#define SIGBREAKF_CTRL_E 0x4000
#define SIGBREAKF_CTRL_F 0x8000

/* exec/ports.h */
#define MP_FLAGS 14
#define MP_SIGBIT 15
#define MP_SIGTASK 16
#define MP_MSGLIST 20
#define MP_SIZE 34
#define MN_REPLYPORT 14
#define MN_LENGTH 18
#define MN_SIZE 20
enum { PA_SIGNAL = 0, PA_SOFTINT = 1, PA_IGNORE = 2 };

/* exec/semaphores.h */
#define SS_SIZE 46

/* exec/memory.h */
#define MEMF_ANY 0
#define MEMF_PUBLIC 0x1
#define MEMF_CHIP 0x2
#define MEMF_FAST 0x4
#define MEMF_LOCAL 0x100
#define MEMF_24BITDMA 0x200
#define MEMF_KICK 0x400
#define MEMF_CLEAR 0x10000
#define MEMF_LARGEST 0x20000
#define MEMF_REVERSE 0x40000
#define MEMF_TOTAL 0x80000

/* dos/dosextens.h: Process */
#define PR_MSGPORT 92
#define PR_SEGLIST 128
#define PR_STACKSIZE 132
#define PR_TASKNUM 140
#define PR_STACKBASE 144
#define PR_RESULT2 148
#define PR_CURRENTDIR 152
#define PR_CIS 156
#define PR_COS 160
#define PR_CONSOLETASK 164
#define PR_FILESYSTEMTASK 168
#define PR_CLI 172
#define PR_RETURNADDR 176
#define PR_WINDOWPTR 184
#define PR_HOMEDIR 188
#define PR_FLAGS 192
#define PR_ARGUMENTS 204
#define PR_LOCALVARS 208
#define PR_CES 224
#define PR_SIZE 228

/* dos/dosextens.h: CommandLineInterface */
#define CLI_RESULT2 0
#define CLI_SETNAME 4
#define CLI_COMMANDDIR 8
#define CLI_RETURNCODE 12
#define CLI_COMMANDNAME 16
#define CLI_FAILLEVEL 20
#define CLI_PROMPT 24
#define CLI_STANDARDINPUT 28
#define CLI_CURRENTINPUT 32
#define CLI_COMMANDFILE 36
#define CLI_INTERACTIVE 40
#define CLI_BACKGROUND 44
#define CLI_CURRENTOUTPUT 48
#define CLI_DEFAULTSTACK 52
#define CLI_STANDARDOUTPUT 56
#define CLI_MODULE 60
#define CLI_SIZE 64

/* dos/dosextens.h: FileHandle */
#define FH_LINK 0
#define FH_PORT 4
#define FH_TYPE 8
#define FH_BUF 12
#define FH_POS 16
#define FH_END 20
#define FH_FUNCS 24
#define FH_FUNC2 28
#define FH_FUNC3 32
#define FH_ARGS 36
#define FH_ARG2 40
#define FH_SIZE 44

/* dos/dosextens.h: DosLibrary */
#define DL_ROOT 34
#define DL_SIZE 70

/* dos/dos.h */
#define MODE_OLDFILE 1005
#define MODE_NEWFILE 1006
#define MODE_READWRITE 1004
#define SHARED_LOCK -2
#define EXCLUSIVE_LOCK -1
#define OFFSET_BEGINNING -1
#define OFFSET_CURRENT 0
#define OFFSET_END 1
#define DOSTRUE 0xFFFFFFFF
#define DOSFALSE 0

/* FileInfoBlock */
#define FIB_DISKKEY 0
#define FIB_DIRENTRYTYPE 4
#define FIB_FILENAME 8
#define FIB_PROTECTION 116
#define FIB_ENTRYTYPE 120
#define FIB_SIZE_ 124
#define FIB_NUMBLOCKS 128
#define FIB_DATE 132
#define FIB_COMMENT 144
#define FIB_SIZE 260
#define ST_ROOT 1
#define ST_USERDIR 2
#define ST_FILE -3

/* FileLock */
#define FL_LINK 0
#define FL_KEY 4
#define FL_ACCESS 8
#define FL_TASK 12
#define FL_VOLUME 16
#define FL_SIZE 20

/* DateStamp */
#define DS_DAYS 0
#define DS_MINUTE 4
#define DS_TICK 8

/* dos errors */
#define ERROR_NO_FREE_STORE 103
#define ERROR_BAD_TEMPLATE 114
#define ERROR_REQUIRED_ARG_MISSING 116
#define ERROR_TOO_MANY_ARGS 118
#define ERROR_LINE_TOO_LONG 120
#define ERROR_OBJECT_IN_USE 202
#define ERROR_OBJECT_EXISTS 203
#define ERROR_DIR_NOT_FOUND 204
#define ERROR_OBJECT_NOT_FOUND 205
#define ERROR_ACTION_NOT_KNOWN 209
#define ERROR_OBJECT_WRONG_TYPE 212
#define ERROR_DIRECTORY_NOT_EMPTY 216
#define ERROR_DEVICE_NOT_MOUNTED 218
#define ERROR_SEEK_ERROR 219
#define ERROR_DISK_FULL 221
#define ERROR_DELETE_PROTECTED 222
#define ERROR_READ_PROTECTED 224
#define ERROR_NOT_A_DOS_DISK 225
#define ERROR_NO_MORE_ENTRIES 232
#define ERROR_FILE_NOT_OBJECT 121
#define ERROR_BAD_NUMBER 115
#define ERROR_INVALID_COMPONENT_NAME 210
#define ERROR_NOT_IMPLEMENTED 236

/* hunk file format */
#define HUNK_UNIT 0x3E7
#define HUNK_NAME 0x3E8
#define HUNK_CODE 0x3E9
#define HUNK_DATA 0x3EA
#define HUNK_BSS 0x3EB
#define HUNK_RELOC32 0x3EC
#define HUNK_RELOC16 0x3ED
#define HUNK_RELOC8 0x3EE
#define HUNK_EXT 0x3EF
#define HUNK_SYMBOL 0x3F0
#define HUNK_DEBUG 0x3F1
#define HUNK_END 0x3F2
#define HUNK_HEADER 0x3F3
#define HUNK_OVERLAY 0x3F5
#define HUNK_BREAK 0x3F6
#define HUNK_DREL32 0x3F7
#define HUNK_DREL16 0x3F8
#define HUNK_DREL8 0x3F9
#define HUNK_LIB 0x3FA
#define HUNK_INDEX 0x3FB
#define HUNK_RELOC32SHORT 0x3FC
#define HUNK_RELRELOC32 0x3FD
#define HUNK_ABSRELOC16 0x3FE

#endif

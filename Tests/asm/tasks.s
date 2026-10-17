; Multitasking test for AmigaMac: real exec tasks created with AddTask.
;  1. messages between tasks (PutMsg / WaitPort / GetMsg / ReplyMsg)
;  2. preemption: two tasks spinning without ever yielding must both run
;  3. timer.device: a 300 ms TR_ADDREQUEST measured with TR_GETSYSTIME
_LVOAllocMem        = -198
_LVOFindTask        = -294
_LVOAddTask         = -282
_LVOSignal          = -324
_LVOWait            = -318
_LVOAllocSignal     = -330
_LVOForbid          = -132
_LVOPermit          = -138
_LVOPutMsg          = -366
_LVOGetMsg          = -372
_LVOReplyMsg        = -378
_LVOWaitPort        = -384
_LVOCreateMsgPort   = -666
_LVODeleteMsgPort   = -672
_LVOCreateIORequest = -654
_LVODeleteIORequest = -660
_LVOOpenDevice      = -444
_LVOCloseDevice     = -450
_LVODoIO            = -456
_LVOOpenLibrary     = -552
_LVOCloseLibrary    = -414
_LVOVPrintf         = -954
_LVODelay           = -198
_LVOSetSignal       = -306
_LVOOutput          = -60
_LVOCreateNewProc   = -498
_LVOInitSemaphore   = -558
_LVOObtainSemaphore = -564
_LVOReleaseSemaphore = -570
NP_Entry  = $800003EB
NP_Output = $800003ED
NP_Name   = $800003F4
NP_CloseOutput = $800003EF

        section code,code
start:  move.l  4.w,a6
        lea     dosname(pc),a1
        moveq   #36,d0
        jsr     _LVOOpenLibrary(a6)
        move.l  d0,dosbase
        beq     .fail
        sub.l   a1,a1
        jsr     _LVOFindTask(a6)
        move.l  d0,parent

;---- 1. messages ---------------------------------------------------------
        lea     t1title(pc),a0
        bsr     print
        jsr     _LVOCreateMsgPort(a6)
        move.l  d0,port1
        lea     name1(pc),a0
        lea     msgchild(pc),a1
        moveq   #0,d0
        bsr     maketask
        moveq   #5-1,d6
.m:     move.l  port1,a0
        jsr     _LVOWaitPort(a6)
        move.l  port1,a0
        jsr     _LVOGetMsg(a6)
        move.l  d0,a2
        lea     args,a1
        move.l  20(a2),(a1)
        lea     t1got(pc),a0
        bsr     print
        move.l  a2,a1
        jsr     _LVOReplyMsg(a6)
        dbf     d6,.m

;---- 2. preemption ---------------------------------------------------------
        lea     t2title(pc),a0
        bsr     print
        moveq   #-1,d0
        jsr     _LVOAllocSignal(a6)
        moveq   #0,d1
        bset    d0,d1
        move.l  d1,donemask
        lea     nameA(pc),a0
        lea     busyA(pc),a1
        moveq   #0,d0
        bsr     maketask
        lea     nameB(pc),a0
        lea     busyB(pc),a1
        moveq   #0,d0
        bsr     maketask
        move.l  dosbase,a6              ; Delay(15): 0.3 s, this task sleeps
        moveq   #15,d1
        jsr     _LVODelay(a6)
        move.l  4.w,a6
        st      stopflag
        lea     args,a1
        move.l  countA,(a1)
        move.l  countB,4(a1)
        lea     t2counts(pc),a0
        bsr     print
.w:     cmp.l   #2,donecount            ; both spinners must end
        beq.s   .both
        move.l  donemask,d0
        jsr     _LVOWait(a6)
        bra.s   .w
.both:  tst.l   countA
        beq.s   .nopre
        tst.l   countB
        beq.s   .nopre
        lea     t2ok(pc),a0
        bsr     print
        bra.s   .t3
.nopre: lea     t2bad(pc),a0
        bsr     print

;---- 3. timer.device ---------------------------------------------------------
.t3:    lea     t3title(pc),a0
        bsr     print
        jsr     _LVOCreateMsgPort(a6)
        move.l  d0,tport
        move.l  d0,a0
        moveq   #40,d0
        jsr     _LVOCreateIORequest(a6)
        move.l  d0,treq
        lea     timername(pc),a0
        moveq   #0,d0                   ; UNIT_MICROHZ
        move.l  treq,a1
        moveq   #0,d1
        jsr     _LVOOpenDevice(a6)
        tst.l   d0
        bne     .notimer
        move.l  treq,a1
        bsr     systime
        move.l  d0,d4                   ; t0 secs
        move.l  d1,d5                   ; t0 micro
        move.l  treq,a1
        move.w  #9,28(a1)               ; TR_ADDREQUEST
        clr.l   32(a1)
        move.l  #300000,36(a1)
        jsr     _LVODoIO(a6)
        move.l  treq,a1
        bsr     systime
        sub.l   d4,d0                   ; elapsed = (s1-s0)*1000 + (u1-u0)/1000
        muls.l  #1000,d0
        sub.l   d5,d1
        divs.l  #1000,d1
        add.l   d1,d0
        lea     args,a1
        move.l  d0,(a1)
        lea     t3got(pc),a0
        bsr     print
        move.l  treq,a1
        jsr     _LVOCloseDevice(a6)
        move.l  treq,a0
        jsr     _LVODeleteIORequest(a6)
        move.l  tport,a0
        jsr     _LVODeleteMsgPort(a6)
        bra.s   .end
.notimer:
        lea     t3bad(pc),a0
        bsr     print

;---- 4. CreateNewProc -----------------------------------------------------------
.end:   bsr     clearsig
        lea     t4title(pc),a0
        bsr     print
        move.l  dosbase,a6
        jsr     _LVOOutput(a6)
        lea     ptags,a0
        move.l  #NP_Entry,(a0)+
        move.l  #procentry,(a0)+
        move.l  #NP_Name,(a0)+
        move.l  #nameP,(a0)+
        move.l  #NP_Output,(a0)+
        move.l  d0,(a0)+
        move.l  #NP_CloseOutput,(a0)+
        clr.l   (a0)+
        clr.l   (a0)
        move.l  #ptags,d1
        jsr     _LVOCreateNewProc(a6)
        move.l  4.w,a6
        tst.l   d0
        beq.s   .noproc
        move.l  donemask,d0             ; the process signals when it is done
        jsr     _LVOWait(a6)
        bra.s   .t5
.noproc:
        lea     t4bad(pc),a0
        bsr     print

;---- 5. semaphore contention ------------------------------------------------------
.t5:    bsr     clearsig
        lea     t5title(pc),a0
        bsr     print
        lea     sem,a0
        jsr     _LVOInitSemaphore(a6)
        lea     sem,a0
        jsr     _LVOObtainSemaphore(a6)
        lea     nameS(pc),a0
        lea     semchild(pc),a1
        moveq   #0,d0
        bsr     maketask
        move.l  dosbase,a6              ; the child must stay blocked meanwhile
        moveq   #5,d1
        jsr     _LVODelay(a6)
        move.l  4.w,a6
        st      released
        lea     sem,a0
        jsr     _LVOReleaseSemaphore(a6)
        move.l  donemask,d0
        jsr     _LVOWait(a6)
        lea     t5ok(pc),a0
        cmp.l   #1,semresult
        beq.s   .s5
        lea     t5bad(pc),a0
.s5:    bsr     print

        move.l  dosbase,a1
        jsr     _LVOCloseLibrary(a6)
        moveq   #0,d0
        rts
.fail:  moveq   #20,d0
        rts

; forget a stale "done" signal (both spinners of test 2 signal)
clearsig:
        moveq   #0,d0
        move.l  donemask,d1
        jmp     _LVOSetSignal(a6)

; TR_GETSYSTIME on the request in A1 -> D0 secs, D1 micro
systime:
        move.w  #10,28(a1)
        move.l  a1,-(sp)
        jsr     _LVODoIO(a6)
        move.l  (sp)+,a1
        move.l  32(a1),d0
        move.l  36(a1),d1
        rts

; print(A0 = format, args in 'args'), keeps all registers
print:  movem.l d0-d2/a0-a1/a6,-(sp)
        move.l  dosbase,a6
        move.l  a0,d1
        move.l  #args,d2
        jsr     _LVOVPrintf(a6)
        movem.l (sp)+,d0-d2/a0-a1/a6
        rts

; maketask(A0 = name, A1 = initial PC, D0 = priority) -> D0 = task
maketask:
        movem.l d2-d4/a2-a3,-(sp)
        move.l  a0,a2
        move.l  a1,a3
        move.l  d0,d4
        move.l  #92+4096,d0
        move.l  #$10001,d1              ; MEMF_PUBLIC|MEMF_CLEAR
        jsr     _LVOAllocMem(a6)
        move.l  d0,a1
        move.b  #1,8(a1)                ; NT_TASK
        move.b  d4,9(a1)                ; ln_Pri
        move.l  a2,10(a1)               ; ln_Name
        lea     92(a1),a0
        move.l  a0,58(a1)               ; tc_SPLower
        lea     4096(a0),a0
        move.l  a0,62(a1)               ; tc_SPUpper
        move.l  a0,54(a1)               ; tc_SPReg
        lea     74(a1),a0               ; NewList(tc_MemEntry)
        move.l  a0,d1
        addq.l  #4,d1
        move.l  d1,(a0)
        clr.l   4(a0)
        move.l  a0,8(a0)
        move.l  a3,a2                   ; initialPC
        sub.l   a3,a3                   ; finalPC: default (task ends)
        jsr     _LVOAddTask(a6)
        movem.l (sp)+,d2-d4/a2-a3
        rts

;---- child tasks -------------------------------------------------------------

; a DOS process started by CreateNewProc, writing to its parent's output
procentry:
        move.l  4.w,a6
        lea     t4got(pc),a0
        bsr     print
        move.l  parent,a1
        move.l  donemask,d0
        jsr     _LVOSignal(a6)
        moveq   #0,d0
        rts

; must get the semaphore only after the parent released it. A plain exec
; task may not call dos.library: it reports through 'semresult'.
semchild:
        move.l  4.w,a6
        lea     sem,a0
        jsr     _LVOObtainSemaphore(a6)
        moveq   #2,d0
        tst.b   released
        beq.s   .bad
        moveq   #1,d0
.bad:   move.l  d0,semresult
        lea     sem,a0
        jsr     _LVOReleaseSemaphore(a6)
        move.l  parent,a1
        move.l  donemask,d0
        jsr     _LVOSignal(a6)
        rts

; sends 10, 20, 30, 40, 50 to port1, waiting for each reply
msgchild:
        move.l  4.w,a6
        jsr     _LVOCreateMsgPort(a6)
        move.l  d0,a4
        moveq   #1,d7
.loop:  lea     msg,a1
        move.l  a4,14(a1)               ; mn_ReplyPort
        move.w  #24,18(a1)              ; mn_Length
        move.l  d7,d0
        mulu.w  #10,d0
        move.l  d0,20(a1)
        move.l  port1,a0
        jsr     _LVOPutMsg(a6)
        move.l  a4,a0
        jsr     _LVOWaitPort(a6)
        move.l  a4,a0
        jsr     _LVOGetMsg(a6)
        addq.l  #1,d7
        cmp.l   #5,d7
        ble.s   .loop
        move.l  a4,a0
        jsr     _LVODeleteMsgPort(a6)
        rts

; spin, counting, until stopflag: never calls the OS, so only preemption
; lets the other tasks run
busyA:  lea     countA,a0
        bra.s   busy
busyB:  lea     countB,a0
busy:   addq.l  #1,(a0)
        tst.b   stopflag
        beq.s   busy
        move.l  4.w,a6
        jsr     _LVOForbid(a6)
        addq.l  #1,donecount
        jsr     _LVOPermit(a6)
        move.l  parent,a1
        move.l  donemask,d0
        jsr     _LVOSignal(a6)
        rts

dosname:   dc.b "dos.library",0
timername: dc.b "timer.device",0
name1:     dc.b "msgchild",0
nameA:     dc.b "busyA",0
nameB:     dc.b "busyB",0
t1title:   dc.b "1. Messages entre tâches",10,0
t1got:     dc.b "   reçu %ld",10,0
t2title:   dc.b "2. Préemption (deux tâches qui ne cèdent jamais)",10,0
t2counts:  dc.b "   busyA a compté %ld, busyB a compté %ld",10,0
t2ok:      dc.b "   préemption OK",10,0
t2bad:     dc.b "   ÉCHEC : une tâche n'a jamais tourné",10,0
t3title:   dc.b "3. timer.device : attente de 300 ms",10,0
t3got:     dc.b "   mesuré : %ld ms",10,0
t3bad:     dc.b "   ÉCHEC : OpenDevice timer.device",10,0
nameP:     dc.b "TestProcess",0
nameS:     dc.b "semchild",0
t4title:   dc.b "4. CreateNewProc",10,0
t4got:     dc.b "   bonjour depuis un processus DOS créé par CreateNewProc",10,0
t4bad:     dc.b "   ÉCHEC : CreateNewProc",10,0
t5title:   dc.b "5. Sémaphore disputé",10,0
t5ok:      dc.b "   la tâche a attendu la libération du sémaphore : OK",10,0
t5bad:     dc.b "   ÉCHEC : sémaphore obtenu alors qu'il était tenu",10,0
        even

        section vars,bss
dosbase:   ds.l 1
parent:    ds.l 1
port1:     ds.l 1
tport:     ds.l 1
treq:      ds.l 1
donemask:  ds.l 1
donecount: ds.l 1
countA:    ds.l 1
countB:    ds.l 1
stopflag:  ds.b 2
args:      ds.l 8
msg:       ds.b 24
ptags:     ds.l 10
sem:       ds.b 46
released:  ds.b 2
semresult: ds.l 1

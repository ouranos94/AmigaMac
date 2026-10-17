; Spin: prints a dot every 0.1 s for 10 s; Ctrl-C (or BREAK) stops it.
_LVOOpenLibrary  = -552
_LVOCloseLibrary = -414
_LVOSetSignal    = -306
_LVOPutStr       = -948
_LVODelay        = -198
SIGBREAKF_CTRL_C = $1000

        section code,code
start:  move.l  4.w,a6
        lea     dosname(pc),a1
        moveq   #36,d0
        jsr     _LVOOpenLibrary(a6)
        move.l  d0,a5
        moveq   #100-1,d7
.loop:  move.l  a5,a6
        moveq   #5,d1
        jsr     _LVODelay(a6)
        move.l  4.w,a6
        moveq   #0,d0
        move.l  #SIGBREAKF_CTRL_C,d1
        jsr     _LVOSetSignal(a6)
        btst    #12,d0
        bne.s   .brk
        move.l  a5,a6
        lea     dot(pc),a0
        move.l  a0,d1
        jsr     _LVOPutStr(a6)
        dbf     d7,.loop
        lea     done(pc),a0
        bra.s   .out
.brk:   lea     brk(pc),a0
.out:   move.l  a5,a6
        move.l  a0,d1
        jsr     _LVOPutStr(a6)
        move.l  a5,a1
        move.l  4.w,a6
        jsr     _LVOCloseLibrary(a6)
        moveq   #0,d0
        rts
dosname: dc.b "dos.library",0
dot:     dc.b ".",0
done:    dc.b " terminé",10,0
brk:     dc.b " ***Break",10,0

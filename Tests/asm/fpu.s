; FPUAsm: 68881 instructions a C compiler rarely emits, with known answers.
; vasmm68k_mot -Fhunkexe -m68020 -m68881 -nosym -o System/C/FPUAsm Tests/asm/fpu.s
_LVOOpenLibrary  = -552
_LVOCloseLibrary = -414
_LVOVPrintf      = -954

        section code,code
start:  lea     args,a2
        ; 1. FMOVECR pi * 1e6, rounded to nearest
        fmovecr.x #0,fp0
        fmul.l  #1000000,fp0
        fmove.l fp0,(a2)+                       ; 3141593
        ; 2. extended round trip
        fmovecr.x #0,fp0
        lea     buf,a0
        fmove.x fp0,(a0)
        fmove.l #0,fp1
        fmove.x (a0),fp1
        fcmp.x  fp0,fp1
        fseq    d0
        ext.w   d0
        ext.l   d0
        move.l  d0,(a2)+                        ; -1
        move.l  (a0),(a2)+                      ; $40000000
        move.l  4(a0),(a2)+                     ; $C90FDAA2
        ; 3. packed decimal, k = 5
        fmove.l #12345,fp2
        fmove.p fp2,(a0){#5}
        move.l  (a0),(a2)+                      ; $00040001
        move.l  4(a0),(a2)+                     ; $23450000
        fmove.p (a0),fp3
        fmove.l fp3,(a2)+                       ; 12345
        ; 4. FMOVEM of all eight registers through the stack
        fmove.l #1,fp0
        fmove.l #2,fp1
        fmove.l #3,fp2
        fmove.l #4,fp3
        fmove.l #5,fp4
        fmove.l #6,fp5
        fmove.l #7,fp6
        fmove.l #8,fp7
        fmovem.x fp0-fp7,-(sp)
        fmove.l #0,fp0
        fmove.l #0,fp3
        fmove.l #0,fp7
        fmovem.x (sp)+,fp0-fp7
        fadd.x  fp1,fp0
        fadd.x  fp2,fp0
        fadd.x  fp3,fp0
        fadd.x  fp4,fp0
        fadd.x  fp5,fp0
        fadd.x  fp6,fp0
        fadd.x  fp7,fp0
        fmove.l fp0,(a2)+                       ; 36
        ; 5. FDBF loop
        fmove.l #0,fp0
        moveq   #9,d1
.loop:  fadd.l  #1,fp0
        fdbf    d1,.loop
        fmove.l fp0,(a2)+                       ; 10
        ; 6. FScc after FCMP
        fmove.l #5,fp0
        fcmp.l  #3,fp0
        fsgt    d2
        fslt    d3
        and.l   #$FF,d2
        and.l   #$FF,d3
        move.l  d2,(a2)+                        ; 255
        move.l  d3,(a2)+                        ; 0
        ; 7. rounding modes from FPCR
        fmove.d #2.7,fp0
        fmove.l #$10,fpcr                       ; round to zero
        fmove.l fp0,(a2)+                       ; 2
        fmove.l #0,fpcr
        fmove.l fp0,(a2)+                       ; 3
        ; 8. FSINCOS of pi/2
        fmovecr.x #0,fp0
        fdiv.l  #2,fp0
        fsincos.x fp0,fp1:fp2
        fmul.l  #1000,fp1
        fmul.l  #1000,fp2
        fmove.l fp2,(a2)+                       ; 1000
        fmove.l fp1,(a2)+                       ; 0
        ; 9. FMOVEM of control registers, FBcc
        fmove.l #$20,fpcr
        fmovem.l fpcr/fpsr,-(sp)
        fmove.l #0,fpcr
        move.l  (sp)+,(a2)+                     ; 32
        addq.l  #4,sp
        fmove.l #-1,fp0
        ftst.x  fp0
        moveq   #0,d0
        fbge    .no
        moveq   #1,d0
.no:    move.l  d0,(a2)+                        ; 1
        ; print
        move.l  4.w,a6
        lea     dosname,a1
        moveq   #36,d0
        jsr     _LVOOpenLibrary(a6)
        move.l  d0,a6
        lea     fmt,a0
        move.l  a0,d1
        lea     args,a0
        move.l  a0,d2
        jsr     _LVOVPrintf(a6)
        move.l  a6,a1
        move.l  4.w,a6
        jsr     _LVOCloseLibrary(a6)
        moveq   #0,d0
        rts

        section data,data
dosname: dc.b   "dos.library",0
fmt:    dc.b    "pi %ld ext %ld %08lx %08lx packed %08lx %08lx %ld",10
        dc.b    "fmovem %ld fdbf %ld fscc %ld %ld rnd %ld %ld sincos %ld %ld fpcr %ld fbcc %ld",10,0
        even
args:   ds.l    20
buf:    ds.l    4

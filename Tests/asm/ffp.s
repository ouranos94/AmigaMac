; FFPTest: mathffp.library and mathtrans.library (Motorola Fast Floating Point).
; vasmm68k_mot -Fhunkexe -m68020 -nosym -o System/C/FFPTest Tests/asm/ffp.s
_LVOOpenLibrary  = -552
_LVOVPrintf      = -954
_LVOSPFix  = -30
_LVOSPFlt  = -36
_LVOSPCmp  = -42
_LVOSPMul  = -78
_LVOSPDiv  = -84
_LVOSPSub  = -72
_LVOSPSqrt = -96
_LVOSPSin  = -36
_LVOSPPow  = -90

        section code,code
start:  move.l  4.w,a6
        lea     ffpname,a1
        moveq   #0,d0
        jsr     _LVOOpenLibrary(a6)
        move.l  d0,a5                           ; mathffp
        lea     transname,a1
        moveq   #0,d0
        jsr     _LVOOpenLibrary(a6)
        move.l  d0,a4                           ; mathtrans
        lea     args,a2
        ; 1000 * (7 / 2)
        move.l  a5,a6
        moveq   #7,d0
        jsr     _LVOSPFlt(a6)
        move.l  d0,d2
        moveq   #2,d0
        jsr     _LVOSPFlt(a6)
        move.l  d0,d1                           ; divisor
        move.l  d2,d0                           ; dividend
        jsr     _LVOSPDiv(a6)
        move.l  d0,d2
        move.l  #1000,d0
        jsr     _LVOSPFlt(a6)
        move.l  d2,d1
        jsr     _LVOSPMul(a6)
        jsr     _LVOSPFix(a6)
        move.l  d0,(a2)+                        ; 3500
        ; 10 - 3
        moveq   #10,d0
        jsr     _LVOSPFlt(a6)
        move.l  d0,d2
        moveq   #3,d0
        jsr     _LVOSPFlt(a6)
        move.l  d0,d1
        move.l  d2,d0
        jsr     _LVOSPSub(a6)
        jsr     _LVOSPFix(a6)
        move.l  d0,(a2)+                        ; 7
        ; compare 10 with 3
        moveq   #10,d0
        jsr     _LVOSPFlt(a6)
        move.l  d0,d2
        moveq   #3,d0
        jsr     _LVOSPFlt(a6)
        move.l  d0,d1
        move.l  d2,d0
        jsr     _LVOSPCmp(a6)
        move.l  d0,(a2)+                        ; 1
        ; sqrt(2) * 10000 with mathtrans
        moveq   #2,d0
        jsr     _LVOSPFlt(a6)
        move.l  a4,a6
        jsr     _LVOSPSqrt(a6)
        move.l  d0,d2
        move.l  a5,a6
        move.l  #10000,d0
        jsr     _LVOSPFlt(a6)
        move.l  d2,d1
        jsr     _LVOSPMul(a6)
        jsr     _LVOSPFix(a6)
        move.l  d0,(a2)+                        ; 14142
        ; the raw FFP of 1.0
        moveq   #1,d0
        jsr     _LVOSPFlt(a6)
        move.l  d0,(a2)+                        ; $80000041
        move.l  4.w,a6
        lea     dosname,a1
        moveq   #36,d0
        jsr     _LVOOpenLibrary(a6)
        move.l  d0,a6
        move.l  #fmt,d1
        move.l  #args,d2
        jsr     _LVOVPrintf(a6)
        moveq   #0,d0
        rts

        section data,data
dosname:   dc.b "dos.library",0
ffpname:   dc.b "mathffp.library",0
transname: dc.b "mathtrans.library",0
fmt:       dc.b "ffp %ld %ld cmp %ld sqrt %ld one %08lx",10,0
        even
args:   ds.l    8

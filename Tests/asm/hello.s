; Hello World for AmigaOS: a normal hunk executable, as any C compiler makes.
_LVOOpenLibrary  = -552
_LVOCloseLibrary = -414
_LVOOutput       = -60
_LVOWrite        = -48
_LVOPutStr       = -948
_LVOVPrintf      = -954
_LVOReadArgs     = -798
_LVOFreeArgs     = -858

        section code,code
start:  move.l  4.w,a6
        lea     dosname(pc),a1
        moveq   #36,d0
        jsr     _LVOOpenLibrary(a6)
        tst.l   d0
        beq.w   .nodos
        move.l  d0,a6

        jsr     _LVOOutput(a6)
        move.l  d0,d1
        lea     msg(pc),a0
        move.l  a0,d2
        moveq   #msglen,d3
        jsr     _LVOWrite(a6)

        lea     line2(pc),a0
        move.l  a0,d1
        jsr     _LVOPutStr(a6)

        ; Printf("%ld + %ld = %ld, %s, $%08lx\n", ...)
        lea     args(pc),a0
        move.l  #40,(a0)
        move.l  #2,4(a0)
        move.l  #42,8(a0)
        lea     word(pc),a1
        move.l  a1,12(a0)
        move.l  #$DEADBEEF,16(a0)
        lea     fmt(pc),a1
        move.l  a1,d1
        move.l  a0,d2
        jsr     _LVOVPrintf(a6)

        ; ReadArgs("NAME/A,LOUD/S", array, NULL)
        lea     tpl(pc),a0
        move.l  a0,d1
        lea     array(pc),a0
        move.l  a0,d2
        moveq   #0,d3
        jsr     _LVOReadArgs(a6)
        move.l  d0,d7
        beq.s   .noargs
        lea     array(pc),a0
        lea     args(pc),a1
        move.l  (a0),(a1)
        lea     greet(pc),a2
        tst.l   4(a0)
        beq.s   .quiet
        lea     shout(pc),a2
.quiet: move.l  a2,d1
        move.l  a1,d2
        jsr     _LVOVPrintf(a6)
        move.l  d7,d1
        jsr     _LVOFreeArgs(a6)
.noargs:
        move.l  a6,a1
        move.l  4.w,a6
        jsr     _LVOCloseLibrary(a6)
        moveq   #0,d0
        rts
.nodos: moveq   #20,d0
        rts

dosname: dc.b   "dos.library",0
msg:    dc.b    "Hello, World! (Write)",10
msglen  = *-msg
line2:  dc.b    "Ceci est un vrai programme Amiga 68k (PutStr)",10,0
fmt:    dc.b    "%ld + %ld = %ld, %s, $%08lx (VPrintf)",10,0
word:   dc.b    "RawDoFmt fonctionne",0
tpl:    dc.b    "NAME/A,LOUD/S",0
greet:  dc.b    "Bonjour %s (ReadArgs)",10,0
shout:  dc.b    "BONJOUR %s !!! (ReadArgs /S)",10,0
        even
args:   ds.l    8
array:  ds.l    4

/* BoopsiTest: a 68k subclass of buttongclass, a frameiclass image and a
   propgclass slider talking to IDCMP through ICA_TARGET */
#include <exec/types.h>
#include <intuition/intuition.h>
#include <intuition/classes.h>
#include <intuition/classusr.h>
#include <intuition/gadgetclass.h>
#include <intuition/imageclass.h>
#include <intuition/icclass.h>
#include <utility/tagitem.h>
#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/utility.h>
#include <clib/alib_protos.h>
#include <stdio.h>

static int renders;

/* our class: buttongclass, plus a cross drawn over the button on GM_RENDER */
static ULONG dispatch(__reg("a0") Class *cl, __reg("a2") Object *o, __reg("a1") Msg msg)
{
    ULONG r = DoSuperMethodA(cl, o, msg);
    if (msg->MethodID == GM_RENDER) {
        struct gpRender *gr = (struct gpRender *)msg;
        struct Gadget *g = (struct Gadget *)o;
        renders++;
        SetAPen(gr->gpr_RPort, 3);
        Move(gr->gpr_RPort, g->LeftEdge + 2, g->TopEdge + 2);
        Draw(gr->gpr_RPort, g->LeftEdge + 12, g->TopEdge + g->Height - 3);
        Move(gr->gpr_RPort, g->LeftEdge + 12, g->TopEdge + 2);
        Draw(gr->gpr_RPort, g->LeftEdge + 2, g->TopEdge + g->Height - 3);
    }
    return r;
}

int main(void)
{
    Class *cl = MakeClass(NULL, "buttongclass", NULL, 0, 0);
    if (!cl) { printf("MakeClass a échoué\n"); return 20; }
    cl->cl_Dispatcher.h_Entry = (HOOKFUNC)dispatch;

    Object *frame = NewObject(NULL, "frameiclass", IA_FrameType, FRAME_BUTTON, TAG_DONE);
    struct Gadget *btn = (struct Gadget *)NewObject(cl, NULL, GA_Left, 20, GA_Top, 30, GA_Width, 120, GA_Height, 16,
        GA_Image, (ULONG)frame, GA_ID, 1, GA_RelVerify, TRUE, TAG_DONE);
    struct Gadget *prop = (struct Gadget *)NewObject(NULL, "propgclass", GA_Previous, (ULONG)btn,
        GA_Left, 20, GA_Top, 60, GA_Width, 200, GA_Height, 10, GA_ID, 2, GA_RelVerify, TRUE,
        PGA_Freedom, FREEHORIZ, PGA_Total, 100, PGA_Visible, 10, PGA_Top, 0,
        ICA_TARGET, ICTARGET_IDCMP, TAG_DONE);
    printf("objets: frame=%s bouton=%s prop=%s\n", frame ? "ok" : "NULL", btn ? "ok" : "NULL", prop ? "ok" : "NULL");

    struct Window *w = OpenWindowTags(NULL, WA_Left, 40, WA_Top, 30, WA_Width, 260, WA_Height, 90,
        WA_Title, (ULONG)"BoopsiTest", WA_Gadgets, (ULONG)btn, WA_CloseGadget, TRUE, WA_DragBar, TRUE,
        WA_Activate, TRUE, WA_IDCMP, IDCMP_GADGETUP | IDCMP_IDCMPUPDATE | IDCMP_CLOSEWINDOW, TAG_DONE);
    if (!w) return 20;
    for (int done = 0; !done;) {
        WaitPort(w->UserPort);
        struct IntuiMessage *m;
        while ((m = (struct IntuiMessage *)GetMsg(w->UserPort))) {
            if (m->Class == IDCMP_GADGETUP) printf("GADGETUP id=%d\n", ((struct Gadget *)m->IAddress)->GadgetID);
            if (m->Class == IDCMP_IDCMPUPDATE)
                printf("IDCMPUPDATE PGA_Top=%ld\n", GetTagData(PGA_Top, ~0, (struct TagItem *)m->IAddress));
            if (m->Class == IDCMP_CLOSEWINDOW) done = 1;
            ReplyMsg((struct Message *)m);
        }
    }
    ULONG top = 0;
    GetAttr(PGA_Top, (Object *)prop, &top);
    printf("final: PGA_Top=%lu, rendus de la sous-classe=%d\n", top, renders);
    CloseWindow(w);
    DisposeObject((Object *)prop);
    DisposeObject((Object *)btn);
    DisposeObject(frame);
    printf("FreeClass: %s\n", FreeClass(cl) ? "ok" : "échec");
    return 0;
}

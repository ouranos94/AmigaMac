/* CxTest: a commodity in the usual style - a broker, a hotkey (filter + sender
 * + translate NULL), a key translation (x -> y), ParseIX checks.
 * Built with: tools/vcc -o System/C/CxTest Tests/c/cxtest.c -lamiga -lauto
 */
#include <exec/types.h>
#include <libraries/commodities.h>
#include <devices/inputevent.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/commodities.h>
#include <stdio.h>
#include <string.h>

struct Library *CxBase;
#define EVT_HOTKEY 1

int main(void)
{
    struct MsgPort *port;
    struct NewBroker nb;
    CxObj *broker, *filter, *xfilter;
    struct InputEvent ie_y;
    IX ix;
    LONG err = 0;
    int hotkeys = 0, quit = 0;

    if (!(CxBase = OpenLibrary("commodities.library", 37))) { printf("pas de commodities.library\n"); return 20; }
    printf("ParseIX(\"ctrl alt h\") = %ld", ParseIX("ctrl alt h", &ix));
    printf(" classe %d code %d qual %04x masque %04x\n", ix.ix_Class, ix.ix_Code, ix.ix_Qualifier, ix.ix_QualMask);
    printf("ParseIX(\"n'importe quoi\") = %ld\n", ParseIX("n'importe quoi", &ix));

    port = CreateMsgPort();
    memset(&nb, 0, sizeof nb);
    nb.nb_Version = NB_VERSION;
    nb.nb_Name = "CxTest";
    nb.nb_Title = "Test des commodites";
    nb.nb_Descr = "ctrl alt h, et x devient y";
    nb.nb_Unique = NBU_UNIQUE | NBU_NOTIFY;
    nb.nb_Port = port;
    broker = CxBroker(&nb, &err);
    printf("CxBroker = %s, erreur %ld\n", broker ? "ok" : "NULL", err);
    if (!broker) return 20;
    printf("second CxBroker du meme nom : %s\n", CxBroker(&nb, &err) ? "accepte" : "refuse");

    filter = CxFilter("ctrl alt h");
    AttachCxObj(broker, filter);
    AttachCxObj(filter, CxSender(port, EVT_HOTKEY));
    AttachCxObj(filter, CxTranslate(NULL));

    memset(&ie_y, 0, sizeof ie_y);
    InvertKeyMap('y', &ie_y, NULL);
    xfilter = CxFilter("rawkey x");
    AttachCxObj(broker, xfilter);
    AttachCxObj(xfilter, CxTranslate(&ie_y));
    printf("erreurs des filtres : %ld %ld\n", CxObjError(filter), CxObjError(xfilter));
    ActivateCxObj(broker, 1);
    printf("CxTest pret\n");

    while (!quit) {
        ULONG sig = Wait((1L << port->mp_SigBit) | SIGBREAKF_CTRL_C);
        CxMsg *msg;
        if (sig & SIGBREAKF_CTRL_C) break;
        while ((msg = (CxMsg *)GetMsg(port))) {
            ULONG type = CxMsgType(msg), id = CxMsgID(msg);
            ReplyMsg((struct Message *)msg);
            if (type == CXM_IEVENT && id == EVT_HOTKEY) { printf("raccourci ctrl alt h recu\n"); if (++hotkeys == 2) quit = 1; }
            if (type == CXM_COMMAND) {
                printf("commande %ld\n", id);
                if (id == CXCMD_KILL) quit = 1;
                if (id == CXCMD_DISABLE) ActivateCxObj(broker, 0);
                if (id == CXCMD_ENABLE) ActivateCxObj(broker, 1);
            }
        }
    }
    DeleteCxObjAll(broker);
    DeleteMsgPort(port);
    CloseLibrary(CxBase);
    printf("CxTest fini\n");
    return 0;
}

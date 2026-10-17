/* prints NDK structure layouts as the Amiga compiler sees them */
#include <stdio.h>
#include <stddef.h>
#include <graphics/gfx.h>
#include <graphics/rastport.h>
#include <graphics/text.h>
#include <graphics/view.h>
#include <graphics/gfxbase.h>
#include <graphics/clip.h>
#include <graphics/layers.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <intuition/intuitionbase.h>
#include <intuition/classes.h>
#include <intuition/classusr.h>
#include <intuition/gadgetclass.h>
#include <intuition/imageclass.h>
#include <intuition/cghooks.h>
#include <devices/inputevent.h>
#include <utility/hooks.h>
#include <utility/tagitem.h>
#include <utility/date.h>
#include <libraries/gadtools.h>
#include <workbench/workbench.h>
#include <workbench/startup.h>
#include <libraries/asl.h>
#define O(s, f) printf("#define %s_%s %d\n", #s, #f, (int)offsetof(struct s, f))
#define S(s) printf("#define %s_SIZEOF %d\n", #s, (int)sizeof(struct s))
int main(void) {
    O(BitMap, BytesPerRow); O(BitMap, Rows); O(BitMap, Flags); O(BitMap, Depth); O(BitMap, Planes); S(BitMap);
    O(RastPort, Layer); O(RastPort, BitMap); O(RastPort, AreaPtrn); O(RastPort, TmpRas); O(RastPort, AreaInfo);
    O(RastPort, Mask); O(RastPort, FgPen); O(RastPort, BgPen); O(RastPort, AOlPen); O(RastPort, DrawMode);
    O(RastPort, AreaPtSz); O(RastPort, Flags); O(RastPort, LinePtrn); O(RastPort, cp_x); O(RastPort, cp_y);
    O(RastPort, Font); O(RastPort, AlgoStyle); O(RastPort, TxFlags); O(RastPort, TxHeight); O(RastPort, TxWidth);
    O(RastPort, TxBaseline); O(RastPort, TxSpacing); S(RastPort);
    O(TextAttr, ta_Name); O(TextAttr, ta_YSize); O(TextAttr, ta_Style); O(TextAttr, ta_Flags);
    O(TextFont, tf_YSize); O(TextFont, tf_Style); O(TextFont, tf_Flags); O(TextFont, tf_XSize); O(TextFont, tf_Baseline);
    O(TextFont, tf_BoldSmear); O(TextFont, tf_Accessors); O(TextFont, tf_LoChar); O(TextFont, tf_HiChar);
    O(TextFont, tf_CharData); O(TextFont, tf_Modulo); O(TextFont, tf_CharLoc); O(TextFont, tf_CharSpace); O(TextFont, tf_CharKern); S(TextFont);
    O(TextExtent, te_Width); O(TextExtent, te_Height); O(TextExtent, te_Extent); S(TextExtent);
    O(ViewPort, ColorMap); O(ViewPort, DWidth); O(ViewPort, DHeight); O(ViewPort, DxOffset); O(ViewPort, DyOffset);
    O(ViewPort, Modes); O(ViewPort, RasInfo); S(ViewPort);
    O(ColorMap, Flags); O(ColorMap, Type); O(ColorMap, Count); O(ColorMap, ColorTable); S(ColorMap);
    O(AreaInfo, VctrTbl); O(AreaInfo, VctrPtr); O(AreaInfo, FlagTbl); O(AreaInfo, FlagPtr); O(AreaInfo, Count);
    O(AreaInfo, MaxCount); O(AreaInfo, FirstX); O(AreaInfo, FirstY); S(AreaInfo);
    O(TmpRas, RasPtr); O(TmpRas, Size); S(TmpRas);
    O(GfxBase, ActiView); O(GfxBase, TextFonts); O(GfxBase, DefaultFont); O(GfxBase, Modes); O(GfxBase, DisplayFlags);
    O(GfxBase, MaxDisplayRow); O(GfxBase, NormalDisplayRows); O(GfxBase, NormalDisplayColumns);
    O(GfxBase, ChipRevBits0); O(GfxBase, MemType); S(GfxBase);
    O(Layer, front); O(Layer, back); O(Layer, ClipRect); O(Layer, rp); O(Layer, bounds); O(Layer, Flags);
    O(Layer, SuperBitMap); O(Layer, Window); O(Layer, Scroll_X); O(Layer, Scroll_Y); O(Layer, LayerInfo);
    O(Layer, Lock); O(Layer, BackFill); O(Layer, ClipRegion); O(Layer, Width); O(Layer, Height); S(Layer);
    S(Layer_Info);
    O(Screen, NextScreen); O(Screen, FirstWindow); O(Screen, LeftEdge); O(Screen, TopEdge); O(Screen, Width);
    O(Screen, Height); O(Screen, MouseY); O(Screen, MouseX); O(Screen, Flags); O(Screen, Title); O(Screen, DefaultTitle);
    O(Screen, BarHeight); O(Screen, BarVBorder); O(Screen, BarHBorder); O(Screen, MenuVBorder); O(Screen, MenuHBorder);
    O(Screen, WBorTop); O(Screen, WBorLeft); O(Screen, WBorRight); O(Screen, WBorBottom); O(Screen, Font);
    O(Screen, ViewPort); O(Screen, RastPort); O(Screen, BitMap); O(Screen, LayerInfo); O(Screen, FirstGadget);
    O(Screen, DetailPen); O(Screen, BlockPen); O(Screen, BarLayer); O(Screen, UserData); S(Screen);
    O(Window, NextWindow); O(Window, LeftEdge); O(Window, TopEdge); O(Window, Width); O(Window, Height);
    O(Window, MouseY); O(Window, MouseX); O(Window, MinWidth); O(Window, MinHeight); O(Window, MaxWidth); O(Window, MaxHeight);
    O(Window, Flags); O(Window, MenuStrip); O(Window, Title); O(Window, FirstRequest); O(Window, ReqCount);
    O(Window, WScreen); O(Window, RPort); O(Window, BorderLeft); O(Window, BorderTop); O(Window, BorderRight);
    O(Window, BorderBottom); O(Window, BorderRPort); O(Window, FirstGadget); O(Window, Parent); O(Window, Descendant);
    O(Window, Pointer); O(Window, IDCMPFlags); O(Window, UserPort); O(Window, WindowPort); O(Window, MessageKey);
    O(Window, DetailPen); O(Window, BlockPen); O(Window, CheckMark); O(Window, ScreenTitle); O(Window, GZZMouseX);
    O(Window, GZZMouseY); O(Window, GZZWidth); O(Window, GZZHeight); O(Window, ExtData); O(Window, UserData);
    O(Window, WLayer); O(Window, IFont); O(Window, MoreFlags); S(Window);
    O(NewWindow, LeftEdge); O(NewWindow, Width); O(NewWindow, DetailPen); O(NewWindow, IDCMPFlags); O(NewWindow, Flags);
    O(NewWindow, FirstGadget); O(NewWindow, CheckMark); O(NewWindow, Title); O(NewWindow, Screen); O(NewWindow, BitMap);
    O(NewWindow, MinWidth); O(NewWindow, MaxWidth); O(NewWindow, Type); S(NewWindow); O(ExtNewWindow, Extension);
    O(NewScreen, LeftEdge); O(NewScreen, Width); O(NewScreen, Depth); O(NewScreen, DetailPen); O(NewScreen, ViewModes);
    O(NewScreen, Type); O(NewScreen, Font); O(NewScreen, DefaultTitle); O(NewScreen, Gadgets); O(NewScreen, CustomBitMap);
    S(NewScreen); O(ExtNewScreen, Extension);
    O(IntuiMessage, Class); O(IntuiMessage, Code); O(IntuiMessage, Qualifier); O(IntuiMessage, IAddress);
    O(IntuiMessage, MouseX); O(IntuiMessage, MouseY); O(IntuiMessage, Seconds); O(IntuiMessage, Micros);
    O(IntuiMessage, IDCMPWindow); O(IntuiMessage, SpecialLink); S(IntuiMessage);
    O(Gadget, NextGadget); O(Gadget, LeftEdge); O(Gadget, Width); O(Gadget, Flags); O(Gadget, Activation);
    O(Gadget, GadgetType); O(Gadget, GadgetRender); O(Gadget, SelectRender); O(Gadget, GadgetText);
    O(Gadget, MutualExclude); O(Gadget, SpecialInfo); O(Gadget, GadgetID); O(Gadget, UserData); S(Gadget);
    O(IntuiText, FrontPen); O(IntuiText, BackPen); O(IntuiText, DrawMode); O(IntuiText, LeftEdge); O(IntuiText, TopEdge);
    O(IntuiText, ITextFont); O(IntuiText, IText); O(IntuiText, NextText); S(IntuiText);
    O(Border, LeftEdge); O(Border, FrontPen); O(Border, DrawMode); O(Border, Count); O(Border, XY); O(Border, NextBorder); S(Border);
    O(Image, LeftEdge); O(Image, Width); O(Image, Depth); O(Image, ImageData); O(Image, PlanePick); O(Image, PlaneOnOff);
    O(Image, NextImage); S(Image);
    O(Menu, NextMenu); O(Menu, LeftEdge); O(Menu, Width); O(Menu, Flags); O(Menu, MenuName); O(Menu, FirstItem); S(Menu);
    O(MenuItem, NextItem); O(MenuItem, LeftEdge); O(MenuItem, Width); O(MenuItem, Flags); O(MenuItem, MutualExclude);
    O(MenuItem, ItemFill); O(MenuItem, SelectFill); O(MenuItem, Command); O(MenuItem, SubItem); O(MenuItem, NextSelect); S(MenuItem);
    O(PropInfo, Flags); O(PropInfo, HorizPot); O(PropInfo, VertPot); O(PropInfo, HorizBody); O(PropInfo, VertBody); S(PropInfo);
    O(StringInfo, Buffer); O(StringInfo, UndoBuffer); O(StringInfo, BufferPos); O(StringInfo, MaxChars);
    O(StringInfo, DispPos); O(StringInfo, NumChars); O(StringInfo, LongInt); S(StringInfo);
    O(EasyStruct, es_StructSize); O(EasyStruct, es_Flags); O(EasyStruct, es_Title); O(EasyStruct, es_TextFormat);
    O(EasyStruct, es_GadgetFormat); S(EasyStruct);
    O(DrawInfo, dri_Version); O(DrawInfo, dri_NumPens); O(DrawInfo, dri_Pens); O(DrawInfo, dri_Font);
    O(DrawInfo, dri_Depth); O(DrawInfo, dri_Resolution); O(DrawInfo, dri_Flags); S(DrawInfo);
    O(IntuitionBase, ActiveWindow); O(IntuitionBase, ActiveScreen); O(IntuitionBase, FirstScreen);
    O(IntuitionBase, MouseY); O(IntuitionBase, MouseX); O(IntuitionBase, Seconds); O(IntuitionBase, Micros);
    S(IntuitionBase);
    O(Hook, h_MinNode); O(Hook, h_Entry); O(Hook, h_SubEntry); O(Hook, h_Data); S(Hook);
    O(IClass, cl_Dispatcher); O(IClass, cl_Reserved); O(IClass, cl_Super); O(IClass, cl_ID); O(IClass, cl_InstOffset);
    O(IClass, cl_InstSize); O(IClass, cl_UserData); O(IClass, cl_SubclassCount); O(IClass, cl_ObjectCount); O(IClass, cl_Flags); S(IClass);
    O(_Object, o_Node); O(_Object, o_Class); S(_Object);
    O(opSet, MethodID); O(opSet, ops_AttrList); O(opSet, ops_GInfo); S(opSet);
    O(opGet, opg_AttrID); O(opGet, opg_Storage);
    O(opUpdate, opu_AttrList); O(opUpdate, opu_GInfo); O(opUpdate, opu_Flags);
    O(opMember, opam_Object);
    O(gpHitTest, gpht_GInfo); O(gpHitTest, gpht_Mouse);
    O(gpRender, gpr_GInfo); O(gpRender, gpr_RPort); O(gpRender, gpr_Redraw);
    O(gpInput, gpi_GInfo); O(gpInput, gpi_IEvent); O(gpInput, gpi_Termination); O(gpInput, gpi_Mouse); O(gpInput, gpi_TabletData);
    O(gpGoInactive, gpgi_GInfo); O(gpGoInactive, gpgi_Abort);
    O(gpLayout, gpl_GInfo); O(gpLayout, gpl_Initial);
    O(GadgetInfo, gi_Screen); O(GadgetInfo, gi_Window); O(GadgetInfo, gi_Requester); O(GadgetInfo, gi_RastPort);
    O(GadgetInfo, gi_Layer); O(GadgetInfo, gi_Domain); O(GadgetInfo, gi_Pens); O(GadgetInfo, gi_DrInfo); S(GadgetInfo);
    O(InputEvent, ie_NextEvent); O(InputEvent, ie_Class); O(InputEvent, ie_SubClass); O(InputEvent, ie_Code);
    O(InputEvent, ie_Qualifier); O(InputEvent, ie_position); O(InputEvent, ie_TimeStamp); S(InputEvent);
    O(impDraw, imp_RPort); O(impDraw, imp_Offset); O(impDraw, imp_State); O(impDraw, imp_DrInfo); O(impDraw, imp_Dimensions);
    O(impHitTest, imp_Point); O(impHitTest, imp_Dimensions);
    O(impErase, imp_RPort); O(impErase, imp_Offset);
    O(impFrameBox, imp_ContentsBox); O(impFrameBox, imp_FrameBox); O(impFrameBox, imp_DrInfo); O(impFrameBox, imp_FrameFlags);
    O(TagItem, ti_Tag); O(TagItem, ti_Data); S(TagItem);
    O(ClockData, sec); O(ClockData, min); O(ClockData, hour); O(ClockData, mday); O(ClockData, month); O(ClockData, year); O(ClockData, wday); S(ClockData);
    O(NewGadget, ng_LeftEdge); O(NewGadget, ng_TopEdge); O(NewGadget, ng_Width); O(NewGadget, ng_Height);
    O(NewGadget, ng_GadgetText); O(NewGadget, ng_TextAttr); O(NewGadget, ng_GadgetID); O(NewGadget, ng_Flags);
    O(NewGadget, ng_VisualInfo); O(NewGadget, ng_UserData); S(NewGadget);
    O(NewMenu, nm_Type); O(NewMenu, nm_Label); O(NewMenu, nm_CommKey); O(NewMenu, nm_Flags);
    O(NewMenu, nm_MutualExclude); O(NewMenu, nm_UserData); S(NewMenu);
    O(DiskObject, do_Magic); O(DiskObject, do_Version); O(DiskObject, do_Gadget); O(DiskObject, do_Type);
    O(DiskObject, do_DefaultTool); O(DiskObject, do_ToolTypes); O(DiskObject, do_CurrentX); O(DiskObject, do_CurrentY);
    O(DiskObject, do_DrawerData); O(DiskObject, do_ToolWindow); O(DiskObject, do_StackSize); S(DiskObject);
    O(DrawerData, dd_NewWindow); O(DrawerData, dd_CurrentX); O(DrawerData, dd_CurrentY); O(DrawerData, dd_Flags);
    O(DrawerData, dd_ViewModes); S(DrawerData); S(OldDrawerData);
    O(WBStartup, sm_Message); O(WBStartup, sm_Process); O(WBStartup, sm_Segment); O(WBStartup, sm_NumArgs);
    O(WBStartup, sm_ToolWindow); O(WBStartup, sm_ArgList); S(WBStartup);
    O(WBArg, wa_Lock); O(WBArg, wa_Name); S(WBArg);
    O(FileRequester, fr_File); O(FileRequester, fr_Drawer); O(FileRequester, fr_LeftEdge); O(FileRequester, fr_TopEdge);
    O(FileRequester, fr_Width); O(FileRequester, fr_Height); O(FileRequester, fr_NumArgs); O(FileRequester, fr_ArgList);
    O(FileRequester, fr_UserData); O(FileRequester, fr_Pattern); S(FileRequester);
    O(FontRequester, fo_Attr); O(FontRequester, fo_FrontPen); O(FontRequester, fo_BackPen); O(FontRequester, fo_DrawMode);
    O(FontRequester, fo_UserData); S(FontRequester);
    O(AppMessage, am_Message); O(AppMessage, am_Type); O(AppMessage, am_UserData); O(AppMessage, am_ID);
    O(AppMessage, am_NumArgs); O(AppMessage, am_ArgList); O(AppMessage, am_Version); O(AppMessage, am_Class);
    O(AppMessage, am_MouseX); O(AppMessage, am_MouseY); O(AppMessage, am_Seconds); O(AppMessage, am_Micros); S(AppMessage);
    return 0;
}

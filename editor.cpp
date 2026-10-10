#include "editor.h"
#include "annotation.h"
#include "ui.h"
#include <imm.h>
using namespace std;
using namespace Gdiplus;
namespace {
constexpr UINT_PTR repeatTimer=0x5350;
constexpr int initialWidth=1200,initialHeight=800;
const array<Color,8> colors{Color(255,255,0,0),Color(255,255,128,0),Color(255,255,235,0),Color(255,0,180,0),Color(255,0,220,220),Color(255,0,96,255),Color(255,160,64,255),Color(255,255,255,255)};
const array<int,6> tools{-1,Annotation::Stroke,Annotation::Arrow,Annotation::Rectangle,Annotation::Text,Annotation::Mosaic};
const array<wstring,6> toolNames{L"选择",L"画笔",L"箭头",L"矩形",L"文字",L"马赛克"};
struct Layout {
    float width=1200,height=800;
    RectF canvas,input,undo,redo,cancel,save,maximize,close,minus,plus,actual,fit;
    RectF smaller{32,316,32,36},number{74,316,106,36},larger{190,316,32,36},size{40,380,174,12},rgb{132,402,90,32};
    RectF Tool(int i) const {return {32.0f+(i%2)*100,106.0f+(i/2)*48,94,40};}
    RectF Swatch(int i) const {return {32.0f+(i%4)*48,450.0f+(i/4)*44,38,34};}
    RectF Channel(int i) const {return {58,462.0f+i*44,154,12};}
    RectF Dialog() const {return {(width-480)/2,(height-210)/2,480,210};}
    RectF DialogButton(int i) const {auto r=Dialog();return {r.X+20+i*148,r.Y+145,140,40};}
} layout;
struct Editor {
    HWND window=nullptr;
    unique_ptr<Bitmap> source;
    ui::ImageCache imageCache;
    array<unique_ptr<Bitmap>,4> mosaics;
    vector<Annotation> annotations;
    vector<vector<Annotation>> undo,redo;
    vector<Annotation> before;
    Annotation live,draft;
    int tool=Annotation::Stroke,selected=-1,editing=-1,channel=0,slider=-1,handle=-1,repeat=0;
    Color color=colors[0];
    float penSize=4,textSize=28,blockSize=16,zoom=1;
    PointF pan{},panStart{},dragStart{},pointer{};
    POINT mouse{-1,-1},pressed{};
    bool textActive=false,drawing=false,pressing=false,rgb=false,composing=false,changing=false,moving=false,panning=false;
    bool numberActive=false,numberAll=false,confirm=false,space=false,textSelecting=false,swallowReturn=false;
    size_t caret=0,anchor=0;
    wstring numberText,notice;
    function<bool(unique_ptr<Bitmap>&,wstring&)> save;
    function<void()> cancel;
} editor;
void Refresh(){if(editor.window)InvalidateRect(editor.window,nullptr,FALSE);}
float Dpi(){return ui::Scale(editor.window);}
void Relayout(){
    RECT r{};GetClientRect(editor.window,&r);
    const float native=GetDpiForWindow(editor.window)/96.0f;
    const float fitted=min({native,max(1.0f,static_cast<float>(r.right))/900,max(1.0f,static_cast<float>(r.bottom))/640});
    SetPropW(editor.window,L"ScreenShotPlus.UiScale",reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(max(.01f,fitted)*1024)));
    layout.width=r.right/Dpi();layout.height=r.bottom/Dpi();
    const float w=layout.width,h=layout.height;
    layout.close={w-46,20,28,32};layout.maximize={w-82,20,28,32};layout.save={w-260,18,164,38};layout.cancel={w-354,18,82,38};
    layout.redo={w-448,18,82,38};layout.undo={w-542,18,82,38};
    layout.canvas={252,82,w-276,h-132-(editor.textActive?126.0f:0.0f)};layout.input={264,h-164,w-300,100};
    layout.minus={w-354,h-40,30,28};layout.plus={w-250,h-40,30,28};layout.actual={w-204,h-40,74,28};layout.fit={w-120,h-40,96,28};
}
RectF ImageRect(){
    if(!editor.source)return {};const float s=editor.zoom/Dpi(),w=editor.source->GetWidth()*s,h=editor.source->GetHeight()*s;
    return {layout.canvas.X+(layout.canvas.Width-w)/2+editor.pan.X,layout.canvas.Y+(layout.canvas.Height-h)/2+editor.pan.Y,w,h};
}
void ClampPan(){
    if(!editor.source)return;
    const float w=editor.source->GetWidth()*editor.zoom/Dpi(),h=editor.source->GetHeight()*editor.zoom/Dpi();
    const float x=max(0.0f,(w-layout.canvas.Width)/2),y=max(0.0f,(h-layout.canvas.Height)/2);
    editor.pan.X=clamp(editor.pan.X,-x,x);editor.pan.Y=clamp(editor.pan.Y,-y,y);
}
void Fit(){
    if(!editor.source)return;editor.zoom=min({1.0f,layout.canvas.Width*Dpi()/editor.source->GetWidth(),layout.canvas.Height*Dpi()/editor.source->GetHeight()});
    editor.pan={};editor.imageCache.Reset();Refresh();
}
bool ImagePoint(PointF p,PointF& point,bool clampPoint=false){
    if(!editor.source)return false;const auto r=ImageRect();
    if(!clampPoint&&(!layout.canvas.Contains(p)||!r.Contains(p)))return false;
    point={clamp((p.X-r.X)*Dpi()/editor.zoom,0.0f,static_cast<float>(editor.source->GetWidth()-1)),clamp((p.Y-r.Y)*Dpi()/editor.zoom,0.0f,static_cast<float>(editor.source->GetHeight()-1))};return true;
}
void Zoom(float zoom,POINT anchor){
    if(!editor.source||editor.drawing||editor.moving||editor.panning)return;
    const auto old=ImageRect();const PointF image{(anchor.x-old.X)*Dpi()/editor.zoom,(anchor.y-old.Y)*Dpi()/editor.zoom};
    editor.zoom=clamp(zoom,.05f,8.0f);const auto next=ImageRect();
    editor.pan.X+=anchor.x-next.X-image.X*editor.zoom/Dpi();editor.pan.Y+=anchor.y-next.Y-image.Y*editor.zoom/Dpi();ClampPan();editor.imageCache.Reset();Refresh();
}
bool Same(const Annotation& a,const Annotation& b){
    if(a.kind!=b.kind||a.color.GetValue()!=b.color.GetValue()||a.size!=b.size||a.text!=b.text||a.points.size()!=b.points.size())return false;
    for(size_t i=0;i<a.points.size();++i)if(a.points[i].X!=b.points[i].X||a.points[i].Y!=b.points[i].Y)return false;return true;
}
bool Same(const vector<Annotation>& a,const vector<Annotation>& b){if(a.size()!=b.size())return false;for(size_t i=0;i<a.size();++i)if(!Same(a[i],b[i]))return false;return true;}
void Push(vector<vector<Annotation>>& history,vector<Annotation> state){if(history.size()==24)history.erase(history.begin());history.push_back(move(state));}
void BeginChange(){if(!editor.changing){editor.before=editor.annotations;editor.changing=true;}}
void EndChange(){if(!editor.changing)return;if(!Same(editor.before,editor.annotations)){Push(editor.undo,move(editor.before));editor.redo.clear();}editor.before.clear();editor.changing=false;}
void StopRepeat(){KillTimer(editor.window,repeatTimer);editor.repeat=0;}
void CancelGesture(){
    StopRepeat();if(editor.changing){editor.annotations=move(editor.before);editor.changing=false;}
    editor.drawing=editor.moving=editor.panning=editor.pressing=editor.textSelecting=false;editor.slider=-1;editor.live={};
    if(GetCapture()==editor.window)ReleaseCapture();Refresh();
}
PointF InputCaretPoint();
void UpdateIme(){
    if(!editor.window)return;HIMC context=ImmGetContext(editor.window);if(!context)return;
    const auto point=editor.textActive?InputCaretPoint():PointF(layout.input.X+12,layout.input.Y+30);
    COMPOSITIONFORM c{};c.dwStyle=CFS_POINT;c.ptCurrentPos={static_cast<LONG>(point.X*Dpi()),static_cast<LONG>(point.Y*Dpi())};ImmSetCompositionWindow(context,&c);
    CANDIDATEFORM candidate{};candidate.dwStyle=CFS_CANDIDATEPOS;candidate.ptCurrentPos=c.ptCurrentPos;ImmSetCandidateWindow(context,&candidate);ImmReleaseContext(editor.window,context);
}
void CancelText(){editor.textActive=false;editor.editing=-1;editor.draft={};editor.composing=false;Relayout();ClampPan();Refresh();}
void CommitText(){
    if(!editor.textActive)return;BeginChange();
    if(editor.editing>=0){if(editor.draft.text.empty())editor.annotations.erase(editor.annotations.begin()+editor.editing);else editor.annotations[editor.editing]=editor.draft;}
    else if(!editor.draft.text.empty())editor.annotations.push_back(editor.draft);
    EndChange();CancelText();editor.selected=-1;
}
int PropertyKind(){if(editor.textActive)return Annotation::Text;if(editor.selected>=0)return editor.annotations[editor.selected].kind;return editor.tool;}
float PropertySize(){if(editor.textActive)return editor.draft.size;if(editor.selected>=0)return editor.annotations[editor.selected].size;return editor.tool==Annotation::Text?editor.textSize:editor.tool==Annotation::Mosaic?editor.blockSize:editor.penSize;}
int Minimum(){return PropertyKind()==Annotation::Text?8:PropertyKind()==Annotation::Mosaic?8:1;}
int Maximum(){return PropertyKind()==Annotation::Text?160:PropertyKind()==Annotation::Mosaic?32:40;}
void SetSize(float value){
    value=static_cast<float>(clamp(static_cast<int>(lround(value)),Minimum(),Maximum()));const int kind=PropertyKind();
    if(kind==Annotation::Mosaic)value=static_cast<float>(clamp(static_cast<int>(lround(value/8)),1,4)*8);
    if(kind==Annotation::Text)editor.textSize=value;else if(kind==Annotation::Mosaic)editor.blockSize=value;else editor.penSize=value;
    if(editor.textActive)editor.draft.size=value;else if(editor.selected>=0)editor.annotations[editor.selected].size=value;Refresh();
}
void SetColor(Color color){editor.color=color;if(editor.textActive)editor.draft.color=color;else if(editor.selected>=0)editor.annotations[editor.selected].color=color;Refresh();}
void AdjustSize(int step){SetSize(PropertySize()+step*(PropertyKind()==Annotation::Mosaic?8:1));}
void FinishNumber(bool apply){
    if(!editor.numberActive)return;
    if(apply){if(editor.numberText.empty())editor.notice=L"请输入大小数值。";else{int value=0;for(wchar_t c:editor.numberText)value=value*10+(c-L'0');BeginChange();SetSize(static_cast<float>(value));EndChange();editor.notice.clear();}}
    editor.numberActive=false;Refresh();
}
void Undo(bool redo=false){
    FinishNumber(false);CancelGesture();if(editor.textActive){CancelText();return;}
    auto& from=redo?editor.redo:editor.undo;auto& to=redo?editor.undo:editor.redo;
    if(!from.empty()){Push(to,move(editor.annotations));editor.annotations=move(from.back());from.pop_back();}editor.selected=-1;editor.notice.clear();Refresh();
}
int Hit(PointF p){
    Graphics g(editor.source.get());
    for(int pass=0;pass<2;++pass)for(int i=static_cast<int>(editor.annotations.size())-1;i>=0;--i)
        if((editor.annotations[i].kind==Annotation::Mosaic)==(pass==0)&&HitAnnotation(g,editor.annotations[i],p,5*Dpi()/editor.zoom))return i;return -1;
}
void LoadProperties(const Annotation& a){editor.color=a.color;if(a.kind==Annotation::Text)editor.textSize=a.size;else if(a.kind==Annotation::Mosaic)editor.blockSize=a.size;else editor.penSize=a.size;}
void StartText(PointF point,int index=-1){
    CommitText();editor.selected=-1;editor.editing=index;editor.textActive=true;
    if(index>=0){editor.draft=editor.annotations[index];LoadProperties(editor.draft);}else{editor.draft={};editor.draft.kind=Annotation::Text;editor.draft.color=editor.color;editor.draft.size=editor.textSize;editor.draft.points={point};}
    editor.caret=editor.anchor=editor.draft.text.size();Relayout();ClampPan();UpdateIme();Refresh();
}
size_t Previous(const wstring& s,size_t p){if(!p)return 0;--p;if(p&&s[p]>=0xDC00&&s[p]<=0xDFFF&&s[p-1]>=0xD800&&s[p-1]<=0xDBFF)--p;return p;}
size_t Next(const wstring& s,size_t p){if(p>=s.size())return s.size();if(s[p]>=0xD800&&s[p]<=0xDBFF&&p+1<s.size()&&s[p+1]>=0xDC00&&s[p+1]<=0xDFFF)return p+2;return p+1;}
void InsertText(wstring text){
    if(!editor.textActive)return;text.erase(remove_if(text.begin(),text.end(),[](wchar_t c){return c<L' '&&c!=L'\n';}),text.end());
    const size_t first=min(editor.caret,editor.anchor),last=max(editor.caret,editor.anchor),room=1024-(editor.draft.text.size()-(last-first));
    if(text.size()>room){text.resize(room);if(!text.empty()&&text.back()>=0xD800&&text.back()<=0xDBFF)text.pop_back();}
    editor.draft.text.replace(first,last-first,text);editor.caret=editor.anchor=first+text.size();editor.notice.clear();UpdateIme();Refresh();
}
void CopyText(bool cut){
    const size_t first=min(editor.caret,editor.anchor),count=max(editor.caret,editor.anchor)-first;if(!count||!OpenClipboard(editor.window))return;
    const auto s=editor.draft.text.substr(first,count);HGLOBAL data=GlobalAlloc(GMEM_MOVEABLE,(s.size()+1)*sizeof(wchar_t));bool copied=false;
    if(data){if(auto p=GlobalLock(data)){memcpy(p,s.c_str(),(s.size()+1)*sizeof(wchar_t));GlobalUnlock(data);if(EmptyClipboard()&&SetClipboardData(CF_UNICODETEXT,data))copied=true;}if(!copied)GlobalFree(data);}
    CloseClipboard();if(copied&&cut)InsertText(L"");
}
void PasteText(){
    if(!OpenClipboard(editor.window))return;HANDLE data=GetClipboardData(CF_UNICODETEXT);
    if(data)if(const auto p=static_cast<const wchar_t*>(GlobalLock(data))){wstring s;const size_t limit=min<size_t>(GlobalSize(data)/sizeof(wchar_t),1024);for(size_t i=0;i<limit&&p[i];++i)if(p[i]!=L'\r')s+=p[i];GlobalUnlock(data);InsertText(s);}CloseClipboard();
}
bool Dirty(){return !editor.annotations.empty()||(editor.textActive&&!editor.draft.text.empty());}
void ResetContent(){
    CancelGesture();editor.imageCache.Reset();editor.source.reset();for(auto& p:editor.mosaics)p.reset();editor.annotations.clear();editor.undo.clear();editor.redo.clear();editor.before.clear();
    editor.textActive=editor.confirm=editor.numberActive=editor.composing=false;editor.selected=editor.editing=-1;
}
void Cancel(){ShowWindow(editor.window,SW_HIDE);ResetContent();if(editor.cancel)editor.cancel();}
void RequestClose(){FinishNumber(true);CancelGesture();if(Dirty()){editor.confirm=true;Refresh();}else Cancel();}
void Save(){
    if(!editor.source)return;FinishNumber(true);CommitText();CancelGesture();editor.confirm=false;
    auto result=RenderAnnotations(*editor.source,editor.annotations,editor.notice);if(result&&editor.save&&editor.save(result,editor.notice)){ShowWindow(editor.window,SW_HIDE);ResetContent();}Refresh();
}
vector<PointF> Handles(const Annotation& a){
    if(a.points.size()<2)return {};if(a.kind==Annotation::Arrow)return {a.points.front(),a.points.back()};if(a.kind!=Annotation::Rectangle&&a.kind!=Annotation::Mosaic)return {};
    const auto p=a.points.front(),q=a.points.back();const float l=min(p.X,q.X),t=min(p.Y,q.Y),r=max(p.X,q.X),b=max(p.Y,q.Y);return {{l,t},{r,t},{r,b},{l,b}};
}
void SelectDown(PointF point){
    editor.handle=-1;if(editor.selected>=0){auto handles=Handles(editor.annotations[editor.selected]);for(int i=0;i<static_cast<int>(handles.size());++i)if(hypot(point.X-handles[i].X,point.Y-handles[i].Y)<=8*Dpi()/editor.zoom){editor.handle=i;break;}}
    if(editor.handle<0)editor.selected=Hit(point);if(editor.selected>=0){LoadProperties(editor.annotations[editor.selected]);BeginChange();editor.dragStart=point;editor.moving=true;}
}
void MoveSelection(PointF point){
    auto& a=editor.annotations[editor.selected];const auto& old=editor.before[editor.selected];a=old;
    if(editor.handle>=0){if(a.kind==Annotation::Arrow)a.points[editor.handle==0?0:a.points.size()-1]=point;else{const auto handles=Handles(old);a.points={handles[(editor.handle+2)%4],point};}}
    else{float dx=point.X-editor.dragStart.X,dy=point.Y-editor.dragStart.Y,l=old.points[0].X,r=l,t=old.points[0].Y,b=t;for(auto p:old.points){l=min(l,p.X);r=max(r,p.X);t=min(t,p.Y);b=max(b,p.Y);}dx=clamp(dx,-l,static_cast<float>(editor.source->GetWidth()-1)-r);dy=clamp(dy,-t,static_cast<float>(editor.source->GetHeight()-1)-b);for(auto& p:a.points){p.X+=dx;p.Y+=dy;}}
}
void MoveDrawing(PointF point){
    if(editor.live.kind==Annotation::Stroke){if(editor.live.points.size()<100000){const auto last=editor.live.points.back();if(hypot(point.X-last.X,point.Y-last.Y)>=.5f)editor.live.points.push_back(point);}}
    else{if(editor.live.kind==Annotation::Rectangle&&(GetKeyState(VK_SHIFT)&0x8000)){const auto p=editor.live.points.front();const float dx=point.X-p.X,dy=point.Y-p.Y;float side=max(abs(dx),abs(dy));side=min({side,dx<0?p.X:editor.source->GetWidth()-1-p.X,dy<0?p.Y:editor.source->GetHeight()-1-p.Y});point={p.X+(dx<0?-side:side),p.Y+(dy<0?-side:side)};}editor.live.points.back()=point;}
}
void FinishDrawing(){
    if(!editor.drawing)return;editor.drawing=false;const auto& p=editor.live.points;
    if(editor.live.kind==Annotation::Stroke||(p.size()>1&&hypot(p.back().X-p.front().X,p.back().Y-p.front().Y)>=1&&(editor.live.kind==Annotation::Arrow||(abs(p.back().X-p.front().X)>=1&&abs(p.back().Y-p.front().Y)>=1)))){BeginChange();editor.annotations.push_back(move(editor.live));EndChange();}editor.live={};
}
int ChannelValue(int i){return i==0?editor.color.GetR():i==1?editor.color.GetG():editor.color.GetB();}
void MoveSlider(POINT point){
    if(editor.slider==3)SetSize(Minimum()+(point.x-layout.size.X)*(Maximum()-Minimum())/layout.size.Width);
    else if(editor.slider>=0){const auto r=layout.Channel(editor.slider);array<int,3> rgb{editor.color.GetR(),editor.color.GetG(),editor.color.GetB()};rgb[editor.slider]=clamp(static_cast<int>(lround((point.x-r.X)*255/r.Width)),0,255);SetColor(Color(255,static_cast<BYTE>(rgb[0]),static_cast<BYTE>(rgb[1]),static_cast<BYTE>(rgb[2])));}
}
float TextWidth(Graphics& g,const wstring& s){
    if(s.empty())return 0;Font font(L"Microsoft YaHei UI",15,FontStyleRegular,UnitPixel);RectF box;
    StringFormat format(StringFormat::GenericTypographic());format.SetFormatFlags(format.GetFormatFlags()|StringFormatFlagsMeasureTrailingSpaces);
    g.MeasureString(s.c_str(),static_cast<INT>(s.size()),&font,PointF(0,0),&format,&box);return box.Width;
}
vector<size_t> LineStarts(){vector<size_t> lines{0};for(size_t i=0;i<editor.draft.text.size();++i)if(editor.draft.text[i]==L'\n')lines.push_back(i+1);return lines;}
int CaretLine(const vector<size_t>& lines){int line=0;for(int i=0;i<static_cast<int>(lines.size());++i)if(lines[i]<=editor.caret)line=i;return line;}
float TextOffset(Graphics& g,const vector<size_t>& lines){const size_t start=lines[CaretLine(lines)];return max(0.0f,TextWidth(g,editor.draft.text.substr(start,editor.caret-start))-layout.input.Width+36);}
PointF InputCaretPoint(){
    Graphics g(editor.source.get());const auto lines=LineStarts();const int line=CaretLine(lines),first=max(0,line-2);
    return {layout.input.X+12+TextWidth(g,editor.draft.text.substr(lines[line],editor.caret-lines[line]))-TextOffset(g,lines),layout.input.Y+50+(line-first)*24};
}
size_t InputPosition(POINT p){
    Graphics g(editor.source.get());const auto lines=LineStarts();const int first=max(0,CaretLine(lines)-2);
    const int line=clamp(first+static_cast<int>((p.y-layout.input.Y-28)/24),0,static_cast<int>(lines.size())-1);
    const size_t start=lines[line],end=line+1<static_cast<int>(lines.size())?lines[line+1]-1:editor.draft.text.size();const float x=max(0.0f,p.x-layout.input.X-12)+TextOffset(g,lines);
    for(size_t i=start;i<end;){const size_t next=Next(editor.draft.text,i);if(x<(TextWidth(g,editor.draft.text.substr(start,i-start))+TextWidth(g,editor.draft.text.substr(start,next-start)))/2)return i;i=next;}return end;
}
void DrawTextInput(Graphics& g){
    ui::Round(g,layout.input,8,ui::surface);ui::Text(g,L"文字输入 · Enter 完成 / Shift+Enter 换行",{layout.input.X+12,layout.input.Y+3,layout.input.Width-24,22},11,ui::muted);
    const auto lines=LineStarts();const int first=max(0,CaretLine(lines)-2);const float offset=TextOffset(g,lines);const size_t selStart=min(editor.caret,editor.anchor),selEnd=max(editor.caret,editor.anchor);
    const auto saved=g.Save();g.SetClip(RectF(layout.input.X+10,layout.input.Y+27,layout.input.Width-20,72));
    for(int line=first;line<min(first+3,static_cast<int>(lines.size()));++line){
        const size_t start=lines[line],end=line+1<static_cast<int>(lines.size())?lines[line+1]-1:editor.draft.text.size();const float x=layout.input.X+12-offset,y=layout.input.Y+28+(line-first)*24;const size_t a=max(start,selStart),b=min(end,selEnd);
        if(a<b){SolidBrush brush(Color(150,ui::accent.GetR(),ui::accent.GetG(),ui::accent.GetB()));g.FillRectangle(&brush,x+TextWidth(g,editor.draft.text.substr(start,a-start)),y,TextWidth(g,editor.draft.text.substr(a,b-a)),22.0f);}
        Font font(L"Microsoft YaHei UI",15,FontStyleRegular,UnitPixel);SolidBrush ink(ui::foreground);const auto s=editor.draft.text.substr(start,end-start);g.DrawString(s.c_str(),static_cast<INT>(s.size()),&font,PointF(x,y),StringFormat::GenericTypographic(),&ink);
        if(editor.caret>=start&&editor.caret<=end){const float cx=x+TextWidth(g,editor.draft.text.substr(start,editor.caret-start));Pen pen(ui::accent,1);g.DrawLine(&pen,cx,y,cx,y+21);}
    }g.Restore(saved);
}
void DrawCanvas(Graphics& g){
    ui::Round(g,layout.canvas,10,Color(255,18,21,27));if(!editor.source)return;
    const auto r=ImageRect();const auto saved=g.Save();g.SetClip(layout.canvas,CombineModeIntersect);g.SetClip(r,CombineModeIntersect);
    if(editor.zoom<=1&&r.Width<=layout.canvas.Width&&r.Height<=layout.canvas.Height)editor.imageCache.Draw(g,editor.window,*editor.source,r);
    else{
        const float l=max(r.X,layout.canvas.X),t=max(r.Y,layout.canvas.Y),right=min(r.GetRight(),layout.canvas.GetRight()),bottom=min(r.GetBottom(),layout.canvas.GetBottom());
        if(right>l&&bottom>t){g.SetInterpolationMode(InterpolationModeNearestNeighbor);g.SetPixelOffsetMode(PixelOffsetModeHalf);g.DrawImage(editor.source.get(),RectF(l,t,right-l,bottom-t),(l-r.X)*Dpi()/editor.zoom,(t-r.Y)*Dpi()/editor.zoom,(right-l)*Dpi()/editor.zoom,(bottom-t)*Dpi()/editor.zoom,UnitPixel);}
    }
    g.TranslateTransform(r.X,r.Y);const float s=editor.zoom/Dpi();g.ScaleTransform(s,s);g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    for(int i=0;i<static_cast<int>(editor.annotations.size());++i)if(i!=editor.editing)DrawAnnotation(g,editor.annotations[i]);
    if(editor.drawing)DrawAnnotation(g,editor.live);if(editor.textActive)DrawAnnotation(g,editor.draft);
    auto mask=[&](const Annotation& a){
        if(a.kind!=Annotation::Mosaic)return;const int block=clamp(static_cast<int>(a.size)/8,1,4)*8,index=block/8-1;
        if(!editor.mosaics[index])editor.mosaics[index]=BuildMosaic(*editor.source,block,editor.notice);if(editor.mosaics[index])DrawMosaic(g,*editor.mosaics[index],a);
    };
    for(const auto& a:editor.annotations)mask(a);if(editor.drawing)mask(editor.live);
    const Annotation* selection=editor.textActive?&editor.draft:editor.selected>=0?&editor.annotations[editor.selected]:nullptr;
    if(selection){auto box=AnnotationBounds(g,*selection);Pen pen(ui::accent,1/s);pen.SetDashStyle(DashStyleDash);g.DrawRectangle(&pen,box);SolidBrush fill(ui::background);Pen edge(ui::accent,1/s);
        for(auto p:Handles(*selection)){RectF h(p.X-4/s,p.Y-4/s,8/s,8/s);g.FillRectangle(&fill,h);g.DrawRectangle(&edge,h);}}
    g.Restore(saved);
}
void Button(Graphics& g,RectF r,const wstring& text,bool active=false,bool enabled=true){
    if(enabled)ui::Button(g,editor.window,r,text,active,ui::Inside(editor.mouse,r));else{ui::Round(g,r,7,ui::surface);ui::Text(g,text,{r.X+8,r.Y+7,r.Width-12,r.Height-8},13,Color(255,95,107,122));}
}
wstring Hint(){
    if(editor.numberActive)return L"输入数值 · Enter 应用 · Esc 取消";
    if(editor.textActive)return L"文字支持选中、复制粘贴和多行 · Enter 完成 · Esc 放弃本次输入";
    if(ui::Inside(editor.mouse,layout.size)||ui::Inside(editor.mouse,layout.number))return L"拖动滑条快速调大小，也可点击数值直接输入 · − / + 按住连续调整";
    if(ui::Inside(editor.mouse,layout.undo))return L"撤销 Ctrl+Z · 最多保留 24 次修改";
    if(ui::Inside(editor.mouse,layout.redo))return L"重做 Ctrl+Y / Ctrl+Shift+Z";
    if(ui::Inside(editor.mouse,layout.save))return L"保存并返回 Ctrl+S · 原截图保留";
    if(editor.selected>=0)return L"拖动移动标注 · 拖动控制点调整形状 · Delete 删除";
    if(editor.tool==Annotation::Mosaic)return L"拖动框选遮盖区域 · 块越大，细节越少";
    if(editor.tool==-1)return L"点击选择标注 · 双击文字修改 · Delete 删除";
    if(editor.tool==Annotation::Rectangle)return L"拖动绘制矩形 · Shift 绘制正方形";
    return L"滚轮缩放 · 空格+拖动或中键平移 · Esc 取消当前操作";
}
void DrawEditor(HWND hwnd,HDC target=nullptr){
    Relayout();ui::Paint paint(hwnd,target);Graphics g(paint.buffer);ui::Prepare(g,hwnd);
    ui::Text(g,L"编辑截图",{24,20,180,32},23,ui::foreground,true);
    if(layout.width>=1100)ui::Text(g,L"原图保留 · 自由标注",{204,27,220,24},12,ui::muted);
    Button(g,layout.undo,L"撤销",false,!editor.undo.empty()||editor.textActive);Button(g,layout.redo,L"重做",false,!editor.redo.empty()&&!editor.textActive);
    Button(g,layout.cancel,L"取消");Button(g,layout.save,L"保存并返回",true);Button(g,layout.maximize,IsZoomed(hwnd)?L"▣":L"□");Button(g,layout.close,L"×");
    ui::Round(g,{20,82,214,layout.height-132},10,ui::surface);for(int i=0;i<6;++i)Button(g,layout.Tool(i),toolNames[i],editor.tool==tools[i]);
    const int kind=PropertyKind();const bool properties=kind>=0;
    ui::Text(g,editor.selected>=0?L"选中标注属性":L"工具属性",{32,272,190,26},14,ui::foreground,true);
    ui::Text(g,kind==Annotation::Text?L"字号 / 原图 px":kind==Annotation::Mosaic?L"马赛克块 / 原图 px":L"粗细 / 原图 px",{32,294,190,22},11,ui::muted);
    Button(g,layout.smaller,L"−",false,properties);Button(g,layout.larger,L"+",false,properties);Button(g,layout.number,editor.numberActive?editor.numberText+L"│":to_wstring(static_cast<int>(PropertySize()))+L" px",editor.numberActive,properties);
    ui::Round(g,layout.size,6,Color(255,60,70,83));const float fraction=(PropertySize()-Minimum())/(Maximum()-Minimum());
    ui::Round(g,{layout.size.X,layout.size.Y,max(1.0f,layout.size.Width*fraction),layout.size.Height},6,properties?ui::accent:ui::muted);SolidBrush knob(properties?ui::accent:ui::muted);g.FillEllipse(&knob,layout.size.X+layout.size.Width*fraction-8,layout.size.Y-2,16.0f,16.0f);
    ui::Text(g,to_wstring(Minimum()),{32,365,30,16},10,ui::muted);ui::Text(g,to_wstring(Maximum()),{190,365,32,16},10,ui::muted);
    if(kind!=Annotation::Mosaic&&properties){
        ui::Round(g,{32,406,24,24},6,editor.color);ui::Text(g,L"颜色",{64,407,60,24},13);Button(g,layout.rgb,L"RGB",editor.rgb);
        if(!editor.rgb)for(int i=0;i<8;++i){const auto r=layout.Swatch(i);ui::Round(g,r,7,colors[i]);if(editor.color.GetValue()==colors[i].GetValue())ui::Border(g,{r.X-3,r.Y-3,r.Width+6,r.Height+6},9,ui::foreground);}
        else for(int i=0;i<3;++i){
            const auto r=layout.Channel(i);ui::Text(g,array<wstring,3>{L"R",L"G",L"B"}[i],{32,r.Y-5,24,22},12,editor.channel==i?ui::accent:ui::muted);
            array<BYTE,3> base{editor.color.GetR(),editor.color.GetG(),editor.color.GetB()};base[i]=0;Color start(255,base[0],base[1],base[2]);base[i]=255;Color end(255,base[0],base[1],base[2]);LinearGradientBrush brush(PointF(r.X,r.Y),PointF(r.GetRight(),r.Y),start,end);g.FillRectangle(&brush,r);
            const float x=r.X+ChannelValue(i)*r.Width/255;Pen marker(ui::foreground,2);g.DrawLine(&marker,x,r.Y-3,x,r.GetBottom()+3);ui::Text(g,to_wstring(ChannelValue(i)),{178,r.Y+12,36,18},10,ui::muted);
        }
    }else if(kind==Annotation::Mosaic)ui::Text(g,L"8 / 16 / 24 / 32 px\n从原图取色，覆盖区域内标注。",{32,420,190,76},12,ui::muted);
    else ui::Text(g,L"点击图片中的标注，\n即可移动、删除或调整属性。",{32,420,190,76},12,ui::muted);
    DrawCanvas(g);if(editor.textActive)DrawTextInput(g);
    if(editor.source)ui::Text(g,to_wstring(editor.source->GetWidth())+L" × "+to_wstring(editor.source->GetHeight())+L" px",{24,layout.height-37,220,24},12,ui::muted);
    Button(g,layout.minus,L"−");Button(g,layout.plus,L"+");Button(g,layout.actual,L"100%");Button(g,layout.fit,L"适应画布");ui::Text(g,to_wstring(static_cast<int>(lround(editor.zoom*100)))+L"%",{layout.minus.GetRight()+8,layout.height-35,64,22},12);
    ui::Text(g,editor.notice.empty()?Hint():editor.notice,{264,layout.height-38,max(40.0f,layout.width-638),30},11,editor.notice.empty()?ui::muted:ui::danger);
    if(editor.confirm){
        SolidBrush shade(Color(180,0,0,0));g.FillRectangle(&shade,0.0f,0.0f,layout.width,layout.height);const auto r=layout.Dialog();ui::Round(g,r,14,ui::surface);ui::Border(g,r,14,ui::accent);
        ui::Text(g,L"保留这次编辑吗？",{r.X+24,r.Y+22,430,32},22,ui::foreground,true);ui::Text(g,L"图片中有未保存的修改，原截图始终保留。",{r.X+24,r.Y+72,430,48},14,ui::muted);
        Button(g,layout.DialogButton(0),L"保存并返回",true);Button(g,layout.DialogButton(1),L"放弃修改");Button(g,layout.DialogButton(2),L"继续编辑");
    }
}
void Down(HWND hwnd,POINT p,bool middle){
    SetFocus(hwnd);editor.mouse=editor.pressed=p;editor.pressing=true;if(editor.confirm){SetCapture(hwnd);return;}
    if(editor.numberActive&&!ui::Inside(p,layout.number))FinishNumber(true);
    if(editor.textActive&&ui::Inside(p,layout.input)){editor.caret=InputPosition(p);if(!(GetKeyState(VK_SHIFT)&0x8000))editor.anchor=editor.caret;editor.textSelecting=true;SetCapture(hwnd);Refresh();return;}
    if(ui::Inside(p,layout.canvas)&&(middle||(editor.space&&!editor.textActive))){editor.panning=true;editor.panStart=editor.pan;SetCapture(hwnd);return;}
    if(middle){editor.pressing=false;return;}SetCapture(hwnd);
    if(PropertyKind()>=0){
        auto size=layout.size;size.Inflate(8,10);if(ui::Inside(p,size)){editor.slider=3;BeginChange();MoveSlider(p);return;}
        if(ui::Inside(p,layout.smaller)||ui::Inside(p,layout.larger)){editor.repeat=ui::Inside(p,layout.smaller)?-1:1;BeginChange();AdjustSize(editor.repeat);SetTimer(hwnd,repeatTimer,350,nullptr);return;}
        if(editor.rgb&&PropertyKind()!=Annotation::Mosaic)for(int i=0;i<3;++i){auto r=layout.Channel(i);r.Inflate(5,10);if(ui::Inside(p,r)){editor.slider=editor.channel=i;BeginChange();MoveSlider(p);return;}}
    }
    PointF point;if(ImagePoint(editor.pointer,point)){
        editor.notice.clear();if(editor.tool==-1){CommitText();SelectDown(point);}
        else if(editor.tool==Annotation::Text){CommitText();const int hit=Hit(point);StartText(point,hit>=0&&editor.annotations[hit].kind==Annotation::Text?hit:-1);}
        else{CommitText();editor.selected=-1;editor.drawing=true;editor.live={};editor.live.kind=static_cast<Annotation::Kind>(editor.tool);editor.live.color=editor.color;editor.live.size=editor.tool==Annotation::Mosaic?editor.blockSize:editor.penSize;editor.live.points={point};if(editor.tool!=Annotation::Stroke)editor.live.points.push_back(point);}
    }Refresh();
}
void Up(HWND hwnd,POINT p){
    const bool pressed=editor.pressing,drew=editor.drawing,moved=editor.moving,panned=editor.panning,slid=editor.slider>=0,repeated=editor.repeat!=0,selectedText=editor.textSelecting;
    if(drew){PointF point;if(ImagePoint(editor.pointer,point,true))MoveDrawing(point);FinishDrawing();}if(moved){PointF point;if(ImagePoint(editor.pointer,point,true))MoveSelection(point);}
    editor.moving=editor.panning=editor.pressing=editor.textSelecting=false;editor.slider=-1;StopRepeat();EndChange();if(GetCapture()==hwnd)ReleaseCapture();
    if(!pressed||drew||moved||panned||slid||repeated||selectedText){Refresh();return;}
    const auto hit=[&](RectF r){return ui::Inside(p,r)&&ui::Inside(editor.pressed,r);};
    if(editor.confirm){if(hit(layout.DialogButton(0)))Save();else if(hit(layout.DialogButton(1)))Cancel();else if(hit(layout.DialogButton(2))){editor.confirm=false;Refresh();}return;}
    POINT center{static_cast<LONG>(layout.canvas.X+layout.canvas.Width/2),static_cast<LONG>(layout.canvas.Y+layout.canvas.Height/2)};
    if(hit(layout.close)||hit(layout.cancel))RequestClose();else if(hit(layout.save))Save();else if(hit(layout.maximize))ShowWindow(hwnd,IsZoomed(hwnd)?SW_RESTORE:SW_MAXIMIZE);
    else if(hit(layout.undo))Undo();else if(hit(layout.redo)&&!editor.textActive)Undo(true);
    else if(hit(layout.minus))Zoom(editor.zoom/1.2f,center);else if(hit(layout.plus))Zoom(editor.zoom*1.2f,center);else if(hit(layout.actual))Zoom(1,center);else if(hit(layout.fit))Fit();
    else if(PropertyKind()>=0&&hit(layout.number)){editor.numberActive=editor.numberAll=true;editor.numberText=to_wstring(static_cast<int>(PropertySize()));editor.notice.clear();}
    else if(PropertyKind()>=0&&PropertyKind()!=Annotation::Mosaic&&hit(layout.rgb))editor.rgb=!editor.rgb;
    else{
        for(int i=0;i<6;++i)if(hit(layout.Tool(i))){FinishNumber(true);CommitText();editor.selected=-1;editor.tool=tools[i];editor.rgb=false;break;}
        if(!editor.rgb&&PropertyKind()>=0&&PropertyKind()!=Annotation::Mosaic)for(int i=0;i<8;++i)if(hit(layout.Swatch(i))){BeginChange();SetColor(colors[i]);EndChange();break;}
    }Refresh();
}
void MoveCaret(size_t position,bool shift){editor.caret=position;if(!shift)editor.anchor=position;UpdateIme();Refresh();}
LRESULT CALLBACK EditorProc(HWND hwnd,UINT msg,WPARAM wparam,LPARAM lparam){
    if(ui::MotionMessage(hwnd,msg,wparam,lparam))return 0;
    if(msg==WM_LBUTTONDOWN||msg==WM_LBUTTONUP||msg==WM_MOUSEMOVE||msg==WM_LBUTTONDBLCLK)
        editor.pointer={GET_X_LPARAM(lparam)/Dpi(),GET_Y_LPARAM(lparam)/Dpi()};
    switch(msg){
    case WM_PAINT:DrawEditor(hwnd);return 0;
    case WM_PRINTCLIENT:DrawEditor(hwnd,reinterpret_cast<HDC>(wparam));return 0;
    case WM_ERASEBKGND:return 1;
    case WM_CLOSE:RequestClose();return 0;
    case WM_GETMINMAXINFO:{
        auto info=reinterpret_cast<MINMAXINFO*>(lparam);const float dpi=GetDpiForWindow(hwnd)/96.0f;
        MONITORINFO monitor{sizeof(monitor)};GetMonitorInfoW(MonitorFromWindow(hwnd,MONITOR_DEFAULTTONEAREST),&monitor);
        info->ptMaxPosition={monitor.rcWork.left-monitor.rcMonitor.left,monitor.rcWork.top-monitor.rcMonitor.top};info->ptMaxSize={monitor.rcWork.right-monitor.rcWork.left,monitor.rcWork.bottom-monitor.rcWork.top};
        info->ptMinTrackSize={min(static_cast<LONG>(900*dpi),info->ptMaxSize.x),min(static_cast<LONG>(640*dpi),info->ptMaxSize.y)};return 0;
    }
    case WM_NCHITTEST:{
        POINT p{GET_X_LPARAM(lparam),GET_Y_LPARAM(lparam)};ScreenToClient(hwnd,&p);RECT r{};GetClientRect(hwnd,&r);const int edge=static_cast<int>(6*GetDpiForWindow(hwnd)/96.0f);
        if(!IsZoomed(hwnd)){const bool l=p.x<edge,t=p.y<edge,right=p.x>=r.right-edge,b=p.y>=r.bottom-edge;
            if(t&&l)return HTTOPLEFT;if(t&&right)return HTTOPRIGHT;if(b&&l)return HTBOTTOMLEFT;if(b&&right)return HTBOTTOMRIGHT;
            if(l)return HTLEFT;if(right)return HTRIGHT;if(t)return HTTOP;if(b)return HTBOTTOM;}
        p={static_cast<LONG>(p.x/Dpi()),static_cast<LONG>(p.y/Dpi())};return !editor.confirm&&p.y<66&&p.x<layout.undo.X?HTCAPTION:HTCLIENT;
    }
    case WM_SIZE:if(editor.window){Relayout();ClampPan();editor.imageCache.Reset();UpdateIme();Refresh();}return 0;
    case WM_DPICHANGED:{
        RemovePropW(hwnd,L"ScreenShotPlus.UiScale");const auto r=reinterpret_cast<RECT*>(lparam);SetWindowPos(hwnd,nullptr,r->left,r->top,r->right-r->left,r->bottom-r->top,SWP_NOZORDER|SWP_NOACTIVATE);
        Relayout();ClampPan();editor.imageCache.Reset();UpdateIme();Refresh();return 0;
    }
    case WM_LBUTTONDOWN:Down(hwnd,ui::Mouse(hwnd,lparam),false);return 0;
    case WM_MBUTTONDOWN:Down(hwnd,ui::Mouse(hwnd,lparam),true);return 0;
    case WM_LBUTTONUP:case WM_MBUTTONUP:Up(hwnd,ui::Mouse(hwnd,lparam));return 0;
    case WM_LBUTTONDBLCLK:{
        PointF point;if(!editor.confirm&&editor.source&&ImagePoint(editor.pointer,point)){CommitText();const int i=Hit(point);if(i>=0&&editor.annotations[i].kind==Annotation::Text)StartText(point,i);}return 0;
    }
    case WM_MOUSEMOVE:{
        editor.mouse=ui::Mouse(hwnd,lparam);TRACKMOUSEEVENT track{sizeof(track),TME_LEAVE,hwnd,0};TrackMouseEvent(&track);
        if(editor.slider>=0)MoveSlider(editor.mouse);if(editor.panning){editor.pan={editor.panStart.X+editor.mouse.x-editor.pressed.x,editor.panStart.Y+editor.mouse.y-editor.pressed.y};ClampPan();}
        PointF point;if(editor.source&&(editor.drawing||editor.moving)&&ImagePoint(editor.pointer,point,true)){if(editor.drawing)MoveDrawing(point);else MoveSelection(point);}
        if(editor.textSelecting)editor.caret=InputPosition(editor.mouse);Refresh();return 0;
    }
    case WM_MOUSELEAVE:editor.mouse={-1,-1};Refresh();return 0;
    case WM_MOUSEWHEEL:{
        POINT p{GET_X_LPARAM(lparam),GET_Y_LPARAM(lparam)};ScreenToClient(hwnd,&p);p={static_cast<LONG>(p.x/Dpi()),static_cast<LONG>(p.y/Dpi())};
        if(!editor.confirm&&ui::Inside(p,layout.canvas))Zoom(editor.zoom*pow(1.2f,GET_WHEEL_DELTA_WPARAM(wparam)/120.0f),p);return 0;
    }
    case WM_SETCURSOR:
        if(LOWORD(lparam)==HTCLIENT){const bool canvas=ui::Inside(editor.mouse,layout.canvas)&&!editor.confirm;
            SetCursor(LoadCursorW(nullptr,canvas?(editor.panning||editor.space?IDC_HAND:editor.tool==-1?IDC_ARROW:editor.tool==Annotation::Text?IDC_IBEAM:IDC_CROSS):IDC_ARROW));return TRUE;}break;
    case WM_CAPTURECHANGED:if(editor.pressing||editor.changing)CancelGesture();return 0;
    case WM_KILLFOCUS:editor.space=false;CancelGesture();FinishNumber(true);return 0;
    case WM_TIMER:
        if(wparam==repeatTimer&&editor.repeat){
            POINT p{};GetCursorPos(&p);ScreenToClient(hwnd,&p);p={static_cast<LONG>(p.x/Dpi()),static_cast<LONG>(p.y/Dpi())};
            const auto r=editor.repeat<0?layout.smaller:layout.larger;if(ui::Inside(p,r))AdjustSize(editor.repeat);
            SetTimer(hwnd,repeatTimer,60,nullptr);return 0;
        }break;
    case WM_KEYUP:if(wparam==VK_SPACE)editor.space=false;return 0;
    case WM_KEYDOWN:{
        if(editor.composing)break;const bool ctrl=(GetKeyState(VK_CONTROL)&0x8000)!=0,shift=(GetKeyState(VK_SHIFT)&0x8000)!=0;
        if(editor.confirm){if(wparam==VK_ESCAPE){editor.confirm=false;Refresh();}return 0;}
        if(editor.numberActive){
            if(wparam==VK_ESCAPE)FinishNumber(false);else if(wparam==VK_RETURN){FinishNumber(true);editor.swallowReturn=true;}else if(ctrl&&wparam=='A')editor.numberAll=true;
            else if(wparam==VK_BACK||wparam==VK_DELETE){if(editor.numberAll)editor.numberText.clear();else if(!editor.numberText.empty())editor.numberText.pop_back();editor.numberAll=false;Refresh();}
            else if(wparam==VK_UP||wparam==VK_DOWN){FinishNumber(true);BeginChange();AdjustSize(wparam==VK_UP?1:-1);EndChange();}return 0;
        }
        if(wparam==VK_ESCAPE){
            if(editor.drawing||editor.moving||editor.panning)CancelGesture();else if(editor.rgb){editor.rgb=false;Refresh();}else if(editor.textActive)CancelText();else if(editor.selected>=0){editor.selected=-1;Refresh();}else RequestClose();return 0;
        }
        if(ctrl&&wparam=='S'){Save();return 0;}if(ctrl&&wparam=='Z'){Undo(shift);return 0;}if(ctrl&&wparam=='Y'){if(!editor.textActive)Undo(true);return 0;}
        if(editor.textActive){
            if(ctrl&&wparam=='A'){editor.anchor=0;editor.caret=editor.draft.text.size();Refresh();return 0;}
            if(ctrl&&(wparam=='C'||wparam=='X')){CopyText(wparam=='X');return 0;}if(ctrl&&wparam=='V'){PasteText();return 0;}
            const auto& s=editor.draft.text;
            if(wparam==VK_LEFT)MoveCaret(!shift&&editor.caret!=editor.anchor?min(editor.caret,editor.anchor):Previous(s,editor.caret),shift);
            else if(wparam==VK_RIGHT)MoveCaret(!shift&&editor.caret!=editor.anchor?max(editor.caret,editor.anchor):Next(s,editor.caret),shift);
            else if(wparam==VK_HOME){const auto p=editor.caret?s.rfind(L'\n',editor.caret-1):wstring::npos;MoveCaret(ctrl?0:p==wstring::npos?0:p+1,shift);}
            else if(wparam==VK_END){const auto p=s.find(L'\n',editor.caret);MoveCaret(ctrl?s.size():p==wstring::npos?s.size():p,shift);}
            else if(wparam==VK_UP||wparam==VK_DOWN){const auto lines=LineStarts();const int line=CaretLine(lines);const size_t column=editor.caret-lines[line];const int target=clamp(line+(wparam==VK_UP?-1:1),0,static_cast<int>(lines.size())-1);
                const size_t end=target+1<static_cast<int>(lines.size())?lines[target+1]-1:s.size();size_t pos=min(lines[target]+column,end);if(pos<s.size()&&pos>0&&s[pos]>=0xDC00&&s[pos]<=0xDFFF)--pos;MoveCaret(pos,shift);}
            else if(wparam==VK_DELETE){if(editor.caret==editor.anchor)editor.anchor=Next(s,editor.caret);InsertText(L"");}return 0;
        }
        if(wparam==VK_SPACE){editor.space=true;return 0;}
        if(wparam==VK_DELETE&&editor.selected>=0){BeginChange();editor.annotations.erase(editor.annotations.begin()+editor.selected);editor.selected=-1;EndChange();Refresh();return 0;}
        if(editor.rgb&&(wparam==VK_UP||wparam==VK_DOWN)){editor.channel=(editor.channel+(wparam==VK_UP?2:1))%3;Refresh();return 0;}
        if(editor.rgb&&(wparam==VK_LEFT||wparam==VK_RIGHT)){BeginChange();array<int,3> rgb{editor.color.GetR(),editor.color.GetG(),editor.color.GetB()};rgb[editor.channel]=clamp(rgb[editor.channel]+(wparam==VK_LEFT?-1:1),0,255);SetColor(Color(255,static_cast<BYTE>(rgb[0]),static_cast<BYTE>(rgb[1]),static_cast<BYTE>(rgb[2])));EndChange();return 0;}
        if(PropertyKind()>=0&&(wparam==VK_OEM_PLUS||wparam==VK_ADD||wparam==VK_OEM_MINUS||wparam==VK_SUBTRACT)){BeginChange();AdjustSize(wparam==VK_OEM_PLUS||wparam==VK_ADD?1:-1);EndChange();return 0;}return 0;
    }
    case WM_CHAR:
        if(wparam==L'\r'&&editor.swallowReturn){editor.swallowReturn=false;return 0;}
        if(editor.composing||editor.confirm||(GetKeyState(VK_CONTROL)&0x8000))return 0;
        if(editor.numberActive){if(wparam>=L'0'&&wparam<=L'9'){if(editor.numberAll)editor.numberText.clear();editor.numberAll=false;if(editor.numberText.size()<4)editor.numberText+=static_cast<wchar_t>(wparam);Refresh();}return 0;}
        if(editor.textActive){if(wparam==L'\r'){if(GetKeyState(VK_SHIFT)&0x8000)InsertText(L"\n");else CommitText();}
            else if(wparam==VK_BACK){if(editor.caret==editor.anchor)editor.anchor=Previous(editor.draft.text,editor.caret);InsertText(L"");}else if(wparam>=L' '&&wparam!=127)InsertText(wstring(1,static_cast<wchar_t>(wparam)));}return 0;
    case WM_IME_STARTCOMPOSITION:if(editor.textActive&&!editor.confirm){editor.composing=true;UpdateIme();}break;
    case WM_IME_ENDCOMPOSITION:editor.composing=false;break;
    case WM_IME_COMPOSITION:
        if(editor.textActive&&!editor.confirm&&(lparam&GCS_RESULTSTR)){HIMC context=ImmGetContext(hwnd);
            if(context){const LONG length=ImmGetCompositionStringW(context,GCS_RESULTSTR,nullptr,0);if(length>0&&length<=8192){wstring text(static_cast<size_t>(length)/sizeof(wchar_t),L'\0');if(ImmGetCompositionStringW(context,GCS_RESULTSTR,text.data(),length)==length)InsertText(text);}ImmReleaseContext(hwnd,context);}return 0;}break;
    }
    return DefWindowProcW(hwnd,msg,wparam,lparam);
}
}
bool InitEditor(HINSTANCE instance,HWND owner,function<bool(unique_ptr<Bitmap>&,wstring&)> save,function<void()> cancel){
    WNDCLASSEXW cls{sizeof(cls)};cls.hInstance=instance;cls.lpfnWndProc=EditorProc;cls.style=CS_DBLCLKS;cls.lpszClassName=L"ScreenShotPlus.Editor";cls.hCursor=LoadCursorW(nullptr,IDC_ARROW);WNDCLASSEXW existing{sizeof(existing)};
    if(!GetClassInfoExW(instance,cls.lpszClassName,&existing)&&!RegisterClassExW(&cls))return false;
    editor.save=move(save);editor.cancel=move(cancel);editor.window=CreateWindowExW(WS_EX_TOOLWINDOW,cls.lpszClassName,L"ScreenShotPlus 编辑截图",WS_POPUP|WS_THICKFRAME|WS_MINIMIZEBOX|WS_MAXIMIZEBOX,0,0,initialWidth,initialHeight,owner,nullptr,instance,nullptr);
    if(editor.window)ui::Corners(editor.window);return editor.window!=nullptr;
}
bool ShowEditor(Bitmap& image,wstring& error){
    if(!editor.window){error=L"编辑窗口尚未初始化。";return false;}auto copy=RenderAnnotations(image,{},error);if(!copy)return false;
    ResetContent();editor.source=move(copy);editor.live={};editor.draft={};editor.tool=Annotation::Stroke;editor.color=colors[0];editor.penSize=4;editor.textSize=28;editor.blockSize=16;editor.rgb=editor.space=false;editor.notice.clear();editor.pan={};
    if(IsZoomed(editor.window))ShowWindow(editor.window,SW_RESTORE);
    POINT cursor{};GetCursorPos(&cursor);MONITORINFO info{sizeof(info)};GetMonitorInfoW(MonitorFromPoint(cursor,MONITOR_DEFAULTTONEAREST),&info);
    SetWindowPos(editor.window,nullptr,info.rcWork.left,info.rcWork.top,0,0,SWP_NOZORDER|SWP_NOSIZE|SWP_NOACTIVATE);RemovePropW(editor.window,L"ScreenShotPlus.UiScale");const float dpi=GetDpiForWindow(editor.window)/96.0f;
    const int w=min(static_cast<int>(initialWidth*dpi),static_cast<int>(info.rcWork.right-info.rcWork.left)-24),h=min(static_cast<int>(initialHeight*dpi),static_cast<int>(info.rcWork.bottom-info.rcWork.top)-24);
    SetWindowPos(editor.window,nullptr,info.rcWork.left+(info.rcWork.right-info.rcWork.left-w)/2,info.rcWork.top+(info.rcWork.bottom-info.rcWork.top-h)/2,w,h,SWP_NOZORDER|SWP_NOACTIVATE);
    Relayout();Fit();ShowWindow(editor.window,SW_SHOW);FocusEditor();Refresh();return true;
}
void FocusEditor(){if(EditorVisible()){SetForegroundWindow(editor.window);SetFocus(editor.window);}}
bool EditorVisible(){return editor.window&&IsWindowVisible(editor.window);}
void DestroyEditor(){if(editor.window){ResetContent();DestroyWindow(editor.window);editor.window=nullptr;}editor.save={};editor.cancel={};}


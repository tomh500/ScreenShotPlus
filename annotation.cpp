#include "annotation.h"
using namespace std;
using namespace Gdiplus;
void DrawAnnotation(Graphics& g, const Annotation& a) {
    if (a.points.empty()) return;
    if (a.kind == Annotation::Stroke) {
        if (a.points.size() == 1) {
            SolidBrush brush(a.color);
            g.FillEllipse(&brush, a.points[0].X - a.size / 2, a.points[0].Y - a.size / 2, a.size, a.size);
        } else {
            Pen pen(a.color, a.size); pen.SetStartCap(LineCapRound); pen.SetEndCap(LineCapRound); pen.SetLineJoin(LineJoinRound);
            g.DrawLines(&pen, a.points.data(), static_cast<INT>(a.points.size()));
        }
    } else {
        Font font(L"Microsoft YaHei UI", a.size, FontStyleRegular, UnitPixel);
        SolidBrush brush(a.color);
        g.DrawString(a.text.c_str(), static_cast<INT>(a.text.size()), &font, a.points[0], StringFormat::GenericTypographic(), &brush);
    }
}
RectF AnnotationTextBounds(Graphics& g, const Annotation& a) {
    RectF result;
    if (a.kind != Annotation::Text || a.points.empty()) return result;
    Font font(L"Microsoft YaHei UI", a.size, FontStyleRegular, UnitPixel);
    g.MeasureString(a.text.c_str(), static_cast<INT>(a.text.size()), &font, a.points[0], StringFormat::GenericTypographic(), &result);
    return result;
}
unique_ptr<Bitmap> RenderAnnotations(Bitmap& source, const vector<Annotation>& annotations, wstring& error) {
    unique_ptr<Bitmap> result(source.Clone(0, 0, static_cast<INT>(source.GetWidth()), static_cast<INT>(source.GetHeight()), PixelFormat32bppARGB));
    if (!result || result->GetLastStatus() != Ok) { error = L"无法分配编辑图像，请重试。"; return {}; }
    Graphics g(result.get());
    if (g.GetLastStatus() != Ok) { error = L"无法创建标注画布，请重试。"; return {}; }
    g.SetSmoothingMode(SmoothingModeAntiAlias); g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    g.SetClip(Rect(0, 0, static_cast<INT>(source.GetWidth()), static_cast<INT>(source.GetHeight())));
    for (const auto& annotation : annotations) {
        DrawAnnotation(g, annotation);
        if (g.GetLastStatus() != Ok) { error = L"标注绘制失败，编辑内容仍然保留。"; return {}; }
    }
    return result;
}

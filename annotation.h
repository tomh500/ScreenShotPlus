#pragma once
#include "framework.h"
struct Annotation {
    enum Kind { Stroke, Text } kind = Stroke;
    Gdiplus::Color color{255, 255, 0, 0};
    float size = 4;
    std::vector<Gdiplus::PointF> points;
    std::wstring text;
};
void DrawAnnotation(Gdiplus::Graphics& graphics, const Annotation& annotation);
Gdiplus::RectF AnnotationTextBounds(Gdiplus::Graphics& graphics, const Annotation& annotation);
std::unique_ptr<Gdiplus::Bitmap> RenderAnnotations(Gdiplus::Bitmap& source,
    const std::vector<Annotation>& annotations, std::wstring& error);

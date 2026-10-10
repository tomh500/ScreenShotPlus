#pragma once
#include "framework.h"
struct Annotation {
    enum Kind { Stroke, Text, Arrow, Rectangle, Mosaic } kind = Stroke;
    Gdiplus::Color color{255, 255, 0, 0};
    float size = 4;
    std::vector<Gdiplus::PointF> points;
    std::wstring text;
};
void DrawAnnotation(Gdiplus::Graphics& graphics, const Annotation& annotation);
Gdiplus::RectF AnnotationBounds(Gdiplus::Graphics& graphics, const Annotation& annotation);
bool HitAnnotation(Gdiplus::Graphics& graphics, const Annotation& annotation, Gdiplus::PointF point, float tolerance);
std::unique_ptr<Gdiplus::Bitmap> BuildMosaic(Gdiplus::Bitmap& source, int block, std::wstring& error);
void DrawMosaic(Gdiplus::Graphics& graphics, Gdiplus::Bitmap& mosaic, const Annotation& annotation);
Gdiplus::RectF AnnotationTextBounds(Gdiplus::Graphics& graphics, const Annotation& annotation);
std::unique_ptr<Gdiplus::Bitmap> RenderAnnotations(Gdiplus::Bitmap& source,
    const std::vector<Annotation>& annotations, std::wstring& error);

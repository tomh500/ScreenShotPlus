#pragma once
#include "framework.h"
namespace editor {
// 标注绘制在图片自身的像素坐标系中，界面缩放不影响导出结果。
enum class Tool { Brush, Text };
struct Shape {
    Tool tool = Tool::Brush;
    Gdiplus::Color color{255, 255, 255, 255};
    float stroke = 4;
    std::vector<Gdiplus::PointF> points; // 画笔
    Gdiplus::PointF at{};                // 文字左上角
    float size = 32;                     // 文字字号，单位是图片像素
    std::wstring text;
};
// 赤橙黄绿青蓝紫，以及浅色与黑色，作为预设画笔颜色。
inline constexpr int kPresetCount = 9;
Gdiplus::Color PresetColor(int index);
int DefaultStrokeWidth(Gdiplus::Bitmap& image);
float DefaultFontSize(Gdiplus::Bitmap& image);
float NextStrokeWidth(float current);
float NextFontSize(float current, int direction);
// 把标注绘制到目标位图上，供导出与预览复用。
void DrawShape(Gdiplus::Graphics& g, const Shape& shape);
// 按顺序绘制全部标注。source 只读，不参与绘制。
bool Flatten(Gdiplus::Bitmap& source, const std::vector<Shape>& shapes, Gdiplus::Bitmap& target);
// owner 仅作为窗口归属；编辑完成后不论保存或取消都会回调一次。
bool InitEditor(HINSTANCE instance, HWND owner, std::function<void(Gdiplus::Bitmap*, const std::filesystem::path&)> done);
void OpenEditor(Gdiplus::Bitmap& image, const std::filesystem::path& file);
void HideEditor();
bool EditorVisible();
void DestroyEditor();
// 把编辑器状态与坐标系打到 stderr，仅在排查问题时开启。
void SetDiagnostics(bool enabled);
// 以下访问器供测试断言编辑器状态；界面逻辑本身不使用它们。
Gdiplus::Bitmap* Canvas();
Gdiplus::RectF CanvasRect();
Gdiplus::RectF SwatchRect(int index);
int SelectedColorIndex();
size_t ShapeCount();
void UndoLastShape();
void RedoLastShape();
void RequestSave();
}

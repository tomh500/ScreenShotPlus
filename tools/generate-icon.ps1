$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$taskRoot = Split-Path -Parent $PSScriptRoot
$taskSizes = @(16, 20, 24, 32, 40, 48, 64, 128, 256)
$taskImages = [System.Collections.Generic.List[byte[]]]::new()
foreach ($taskSize in $taskSizes) {
    $taskBitmap = [System.Drawing.Bitmap]::new($taskSize, $taskSize)
    $taskGraphics = [System.Drawing.Graphics]::FromImage($taskBitmap)
    $taskGraphics.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $taskGraphics.ScaleTransform($taskSize / 256.0, $taskSize / 256.0)
    $taskPath = [System.Drawing.Drawing2D.GraphicsPath]::new()
    foreach ($taskArc in @(@(8,8,180), @(152,8,270), @(152,152,0), @(8,152,90))) {
        $taskPath.AddArc($taskArc[0], $taskArc[1], 96, 96, $taskArc[2], 90)
    }
    $taskPath.CloseFigure()
    $taskGreen = [System.Drawing.SolidBrush]::new([System.Drawing.Color]::FromArgb(105,232,187))
    $taskDark = [System.Drawing.SolidBrush]::new([System.Drawing.Color]::FromArgb(20,26,35))
    $taskPen = [System.Drawing.Pen]::new($taskDark.Color, 16)
    $taskPen.StartCap = $taskPen.EndCap = [System.Drawing.Drawing2D.LineCap]::Square
    $taskGraphics.FillPath($taskGreen, $taskPath)
    foreach ($taskCorner in @(@(96,64,64,64,64,96), @(160,64,192,64,192,96), @(192,160,192,192,160,192), @(96,192,64,192,64,160))) {
        $taskGraphics.DrawLine($taskPen, $taskCorner[0], $taskCorner[1], $taskCorner[2], $taskCorner[3])
        $taskGraphics.DrawLine($taskPen, $taskCorner[2], $taskCorner[3], $taskCorner[4], $taskCorner[5])
    }
    $taskGraphics.FillRectangle($taskDark, 104,104,48,48)
    $taskStream = [System.IO.MemoryStream]::new()
    $taskBitmap.Save($taskStream, [System.Drawing.Imaging.ImageFormat]::Png)
    $taskImages.Add($taskStream.ToArray())
    $taskStream.Dispose(); $taskPen.Dispose(); $taskDark.Dispose(); $taskGreen.Dispose()
    $taskPath.Dispose(); $taskGraphics.Dispose(); $taskBitmap.Dispose()
}
$taskOutput = [System.IO.MemoryStream]::new()
$taskWriter = [System.IO.BinaryWriter]::new($taskOutput)
$taskWriter.Write([uint16]0); $taskWriter.Write([uint16]1); $taskWriter.Write([uint16]$taskSizes.Length)
$taskOffset = 6 + 16 * $taskSizes.Length
for ($taskIndex = 0; $taskIndex -lt $taskSizes.Length; ++$taskIndex) {
    $taskSize = $taskSizes[$taskIndex]
    $taskDimension = if ($taskSize -eq 256) { 0 } else { $taskSize }
    $taskWriter.Write([byte]$taskDimension); $taskWriter.Write([byte]$taskDimension)
    $taskWriter.Write([uint16]0); $taskWriter.Write([uint16]1); $taskWriter.Write([uint16]32)
    $taskWriter.Write([uint32]$taskImages[$taskIndex].Length); $taskWriter.Write([uint32]$taskOffset)
    $taskOffset += $taskImages[$taskIndex].Length
}
foreach ($taskBytes in $taskImages) { $taskWriter.Write($taskBytes) }
$taskWriter.Flush()
foreach ($taskName in @('ScreenShotPlus.ico','small.ico')) {
    [System.IO.File]::WriteAllBytes((Join-Path $taskRoot $taskName), $taskOutput.ToArray())
}
$taskWriter.Dispose(); $taskOutput.Dispose()

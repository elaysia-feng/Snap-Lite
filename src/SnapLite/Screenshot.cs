using System.Drawing;
using System.Drawing.Imaging;
using System.Windows.Forms;

namespace SnapLite;

public static class Screenshot
{
    public static void FullScreen()
    {
        var bounds = Screen.PrimaryScreen!.Bounds;
        using var bmp = new Bitmap(bounds.Width, bounds.Height);
        using var g = Graphics.FromImage(bmp);
        g.CopyFromScreen(bounds.Location, Point.Empty, bounds.Size);

        Clipboard.SetImage(bmp);
        bmp.Save($"Snap-{DateTime.Now:yyyyMMdd-HHmmss}.png", ImageFormat.Png);
    }
}

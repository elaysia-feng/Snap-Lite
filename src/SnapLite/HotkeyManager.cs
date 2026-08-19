using System.Runtime.InteropServices;
using System.Windows.Forms;

namespace SnapLite;

public class HotkeyManager : NativeWindow, IDisposable
{
    private const int WM_HOTKEY = 0x0312;
    private const uint MOD_NONE = 0;
    private Action? action;

    public HotkeyManager()
    {
        CreateHandle(new CreateParams());
    }

    public void Register(Keys key, Action callback)
    {
        action = callback;
        RegisterHotKey(Handle, 1, MOD_NONE, (uint)key);
    }

    protected override void WndProc(ref Message m)
    {
        if (m.Msg == WM_HOTKEY)
            action?.Invoke();
        base.WndProc(ref m);
    }

    public void Dispose()
    {
        DestroyHandle();
    }

    [DllImport("user32.dll")]
    private static extern bool RegisterHotKey(IntPtr hWnd, int id, uint fsModifiers, uint vk);
}

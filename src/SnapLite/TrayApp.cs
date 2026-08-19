using System;
using System.Drawing;
using System.Windows.Forms;

namespace SnapLite;

public class TrayApp : ApplicationContext
{
    private readonly NotifyIcon tray;

    public TrayApp()
    {
        tray = new NotifyIcon
        {
            Icon = SystemIcons.Application,
            Visible = true,
            Text = "Snap-Lite"
        };

        var menu = new ContextMenuStrip();
        menu.Items.Add("全屏截图", null, (_, _) => Screenshot.FullScreen());
        menu.Items.Add("退出", null, (_, _) => Exit());
        tray.ContextMenuStrip = menu;

        var hotkey = new HotkeyManager();
        hotkey.Register(Keys.F2, () => Screenshot.FullScreen());
    }

    private void Exit()
    {
        tray.Visible = false;
        Application.Exit();
    }
}

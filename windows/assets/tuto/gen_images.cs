using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Imaging;

const int W = 640, H = 360;
string outDir = Path.Combine(Path.GetTempPath(), "tutogen", "assets", "tuto");
Directory.CreateDirectory(outDir);

Color bg      = Color.FromArgb(240, 241, 245);
Color accent  = Color.FromArgb(0, 102, 204);
Color dark    = Color.FromArgb(30, 34, 42);
Color shell   = Color.FromArgb(34, 40, 52);
Color row     = Color.FromArgb(44, 52, 68);
Color green   = Color.FromArgb(34, 197, 94);
Color gray    = Color.FromArgb(122, 130, 144);
Color white   = Color.White;

void Pill(Graphics g, Rectangle r, int rad, Color c)
{
    using var path = new GraphicsPath();
    int d = rad * 2;
    path.AddArc(r.X, r.Y, d, d, 180, 90);
    path.AddArc(r.Right - d, r.Y, d, d, 270, 90);
    path.AddArc(r.Right - d, r.Bottom - d, d, d, 0, 90);
    path.AddArc(r.X, r.Bottom - d, d, d, 90, 90);
    path.CloseFigure();
    using var b = new SolidBrush(c);
    g.FillPath(b, path);
}

GraphicsPath Rounded(Rectangle r, int rad)
{
    var path = new GraphicsPath();
    if (rad <= 0)
    {
        path.AddRectangle(r);
        return path;
    }
    int d = rad * 2;
    path.AddArc(r.X, r.Y, d, d, 180, 90);
    path.AddArc(r.Right - d, r.Y, d, d, 270, 90);
    path.AddArc(r.Right - d, r.Bottom - d, d, d, 0, 90);
    path.AddArc(r.X, r.Bottom - d, d, d, 90, 90);
    path.CloseFigure();
    return path;
}

void Text(Graphics g, string s, float size, FontStyle fs, Color c, float x, float y)
{
    using var f = new Font("Segoe UI", size, fs);
    g.DrawString(s, f, new SolidBrush(c), x, y);
}

void CenterText(Graphics g, string s, float size, FontStyle fs, Color c, float x, float y, float w, float h)
{
    using var f = new Font("Segoe UI", size, fs);
    var sz = g.MeasureString(s, f);
    g.DrawString(s, f, new SolidBrush(c), x + (w - sz.Width) / 2, y + (h - sz.Height) / 2);
}

void DrawVita(Graphics g, float x, float y, float w, float h, Action<Graphics, RectangleF> screen)
{
    using var bezel = Rounded(new Rectangle((int)x, (int)y, (int)w, (int)h), (int)(h * 0.12f));
    g.FillPath(new SolidBrush(Color.FromArgb(42, 46, 56)), bezel);
    var scr = new RectangleF(x + w * 0.09f, y + h * 0.08f, w - w * 0.18f, h - h * 0.16f);
    using var inner = Rounded(new Rectangle((int)scr.X, (int)scr.Y, (int)scr.Width, (int)scr.Height), (int)(h * 0.05f));
    g.SetClip(inner);
    g.FillRectangle(new SolidBrush(Color.FromArgb(222, 229, 238)), scr);
    screen(g, scr);
    g.ResetClip();
    using var b = new SolidBrush(Color.FromArgb(240, 240, 245));
    // d-pad at left of bezel
    float padX = x + w * 0.02f, padY = y + h * 0.45f, pad = w * 0.07f, padT = w * 0.016f;
    g.FillRectangle(b, padX - padT / 2, padY - pad, padT, pad * 2);
    g.FillRectangle(b, padX - pad, padY - padT / 2, pad * 2, padT);
    g.FillRectangle(b, padX - pad / 2, padY - pad / 2, pad, pad);
    // buttons at right: circle/cross/square/triangle
    float bx = x + w - w * 0.09f, by = y + h * 0.45f, br = w * 0.026f, bt = w * 0.018f;
    g.FillEllipse(b, bx - br, by - br, br * 2, br * 2);       // cross (center)
    g.FillEllipse(b, bx - br, by - br * 3.2f, br * 2, br * 2); // up
    g.FillEllipse(b, bx - br * 3.2f, by - br, br * 2, br * 2); // left
    g.FillEllipse(b, bx + br * 1.2f, by - br, br * 2, br * 2); // right
}

void DrawWindow(Graphics g, Rectangle r, string title, bool darkTheme)
{
    var titleBg = darkTheme ? Color.FromArgb(24, 28, 38) : Color.FromArgb(220, 222, 228);
    var body    = darkTheme ? Color.FromArgb(34, 40, 52) : Color.White;
    using var p = Rounded(r, 10);
    g.FillPath(new SolidBrush(titleBg), p);
    using var bodyP = Rounded(new Rectangle(r.X, r.Y + 28, r.Width, r.Height - 28), 0);
    g.FillPath(new SolidBrush(body), bodyP);
    // dots
    g.FillEllipse(new SolidBrush(Color.FromArgb(255, 95, 86)), r.X + 10, r.Y + 9, 10, 10);
    g.FillEllipse(new SolidBrush(Color.FromArgb(255, 189, 46)), r.X + 26, r.Y + 9, 10, 10);
    g.FillEllipse(new SolidBrush(Color.FromArgb(39, 201, 63)), r.X + 42, r.Y + 9, 10, 10);
    Text(g, title, 11f, FontStyle.Bold, titleBg == Color.FromArgb(24, 28, 38) ? Color.Silver : Color.FromArgb(60, 64, 72), r.X + 62, r.Y + 7);
}

void DrawFileChip(Graphics g, float x, float y, float w, float h, string name, bool ok)
{
    var c = ok ? green : accent;
    Pill(g, new Rectangle((int)x, (int)y, (int)w, (int)h), (int)(h * 0.22f), ok ? Color.FromArgb(226, 250, 232) : Color.FromArgb(255, 255, 255));
    using var pen = new Pen(c, 2.5f);
    using var p = Rounded(new Rectangle((int)x, (int)y, (int)w, (int)h), (int)(h * 0.22f));
    g.DrawPath(pen, p);
    // doc icon
    float iw = h * 0.5f, ix = x + h * 0.3f, iy = y + h * 0.25f;
    g.FillRectangle(new SolidBrush(c), ix, iy, iw, iw);
    g.FillRectangle(new SolidBrush(ok ? Color.FromArgb(226, 250, 232) : white), ix + iw * 0.2f, iy + iw * 0.2f, iw * 0.6f, iw * 0.55f);
    Text(g, name, h * 0.26f, FontStyle.Bold, dark, x + h * 0.9f, y + h * 0.24f);
    if (ok)
    {
        g.DrawString("OK", new Font("Segoe UI", h * 0.2f, FontStyle.Bold), new SolidBrush(green), x + w - h * 0.85f, y + h * 0.22f);
    }
}

// ---------- 1.png : viewer / plugin / download ----------
using (var bmp = new Bitmap(W, H))
using (var g = Graphics.FromImage(bmp))
{
    g.SmoothingMode = SmoothingMode.AntiAlias;
    g.TextRenderingHint = System.Drawing.Text.TextRenderingHint.ClearTypeGridFit;
    g.Clear(bg);

    // Laptop
    float lx = 40, ly = 60, lw = 260, lh = 210;
    void LaptopScreen(Graphics gg, RectangleF s)
    {
        gg.FillRectangle(Brushes.White, s);
        Text(gg, "VitaSync", 20f, FontStyle.Bold, accent, s.X + s.Width * 0.18f, s.Y + s.Height * 0.2f);
        Text(gg, "PC", 13f, FontStyle.Regular, gray, s.X + s.Width * 0.18f, s.Y + s.Height * 0.42f);
        Text(gg, "app", 13f, FontStyle.Regular, gray, s.X + s.Width * 0.18f, s.Y + s.Height * 0.55f);
    }
    DrawVita(g, lx, ly, lw, lh, LaptopScreen);
    // laptop base
    g.FillRectangle(new SolidBrush(dark), lx + 10, ly + lh - 4, lw - 20, 16);

    // Vita (right)
    float vx = 350, vy = 40, vw = 250, vh = 250;
    void VitaScreen(Graphics gg, RectangleF s)
    {
        gg.FillRectangle(Brushes.White, s);
        Text(gg, "Vita", 20f, FontStyle.Bold, Color.FromArgb(60, 66, 80), s.X + s.Width * 0.23f, s.Y + s.Height * 0.3f);
    }
    DrawVita(g, vx, vy, vw, vh, VitaScreen);

    // USB cable
    using (var cable = new Pen(Color.FromArgb(90, 100, 115), 5f))
    {
        g.DrawBezier(cable, lx + lw * 0.75f, ly + lh * 0.4f, lx + lw * 0.8f, ly + lh + 30, vx, vy + lh + 30, vx + vw * 0.2f, vy + vh * 0.35f);
    }
    g.FillRectangle(new SolidBrush(dark), lx + lw * 0.7f, ly + lh * 0.3f, 24, 12);
    g.FillRectangle(new SolidBrush(shell), vx + vw * 0.15f, vy + vh * 0.28f, 12, 24);

    // file chip
    DrawFileChip(g, 120, 290, 250, 44, "VitaSync.skprx", false);
    Text(g, "USB", 12f, FontStyle.Bold, gray, 285, 265);

    bmp.Save(Path.Combine(outDir, "1.png"), ImageFormat.Png);
    Console.WriteLine("1.png done");
}

// ---------- 2.png : VitaShell ur0:/tai/ ----------
using (var bmp = new Bitmap(W, H))
using (var g = Graphics.FromImage(bmp))
{
    g.SmoothingMode = SmoothingMode.AntiAlias;
    g.TextRenderingHint = System.Drawing.Text.TextRenderingHint.ClearTypeGridFit;
    g.Clear(bg);

    var win = new Rectangle(80, 30, 480, 300);
    DrawWindow(g, win, "VitaShell", true);

    // path bar
    Pill(g, new Rectangle(win.X + 16, win.Y + 42, win.Width - 32, 30), 12, Color.FromArgb(24, 28, 38));
    Text(g, "ur0:/tai/", 13f, FontStyle.Regular, Color.Silver, win.X + 28, win.Y + 48);

    int ry = win.Y + 86;
    bool firstRowFilled = false;
    void Row(string name, bool highlight)
    {
        var rc = new Rectangle(win.X + 16, ry, win.Width - 32, 44);
        Pill(g, rc, 8, highlight ? Color.FromArgb(0, 102, 204) : row);
        var ic = highlight ? Color.White : Color.FromArgb(150, 160, 175);
        // folder icon
        g.FillRectangle(new SolidBrush(ic), rc.X + 14, rc.Y + 13, 22, 17);
        g.FillRectangle(Brushes.Transparent, rc.X + 14, rc.Y + 13, 22, 6);
        using (var ip = new Pen(ic, 2.6f))
        {
            g.DrawLine(ip, rc.X + 12, rc.Y + 13, rc.X + 14, rc.Y + 13);
        }
        using var nameFont = new Font("Segoe UI", 14f, FontStyle.Regular);
        g.DrawString(name, nameFont, new SolidBrush(highlight ? Color.White : Color.FromArgb(215, 220, 228)), rc.X + 44, rc.Y + 12);
        ry += 54;
    }
    Row("tai", true);
    Row("VitaSync.skprx", false);
    Row("appmgr", false);

    Text(g, "copier VitaSync.skprx ici", 13f, FontStyle.Bold, green, win.X + 16, win.Y + 252);

    bmp.Save(Path.Combine(outDir, "2.png"), ImageFormat.Png);
    Console.WriteLine("2.png done");
}

// ---------- 3.png : config.txt ----------
using (var bmp = new Bitmap(W, H))
using (var g = Graphics.FromImage(bmp))
{
    g.SmoothingMode = SmoothingMode.AntiAlias;
    g.TextRenderingHint = System.Drawing.Text.TextRenderingHint.ClearTypeGridFit;
    g.Clear(bg);

    var win = new Rectangle(90, 28, 460, 300);
    DrawWindow(g, win, "config.txt — Bloc-notes", false);

    float ty = win.Y + 50;
    void ClsLine(string s, bool hi)
    {
        if (hi)
        {
            using var hl = new SolidBrush(Color.FromArgb(222, 250, 232));
            g.FillRectangle(hl, win.X + 10, ty - 4, win.Width - 20, 30);
            using var lPen = new Pen(green, 3f);
            g.DrawLine(lPen, win.X + 10, ty - 4, win.X + 14, ty - 4);
        }
        using var f = new Font("Consolas", 13f, FontStyle.Bold);
        g.DrawString(s, f, new SolidBrush(hi ? Color.FromArgb(20, 120, 60) : Color.FromArgb(90, 96, 106)), win.X + 24, ty);
        ty += 36;
    }
    ClsLine("# taiHEN config (ur0:tai)", false);
    ClsLine("*KERNEL", true);
    ClsLine("ur0:tai/VitaSync.skprx", true);
    ClsLine("*main", false);
    ClsLine("# \u2026autres plugins\u2026", false);

    Text(g, "ajoute ces 2 lignes \u2713", 14f, FontStyle.Bold, green, win.X + 16, win.Y + 248);

    bmp.Save(Path.Combine(outDir, "3.png"), ImageFormat.Png);
    Console.WriteLine("3.png done");
}

// ---------- 4.png : reboot ----------
using (var bmp = new Bitmap(W, H))
using (var g = Graphics.FromImage(bmp))
{
    g.SmoothingMode = SmoothingMode.AntiAlias;
    g.TextRenderingHint = System.Drawing.Text.TextRenderingHint.ClearTypeGridFit;
    g.Clear(bg);

    void VitaScreen4(Graphics gg, RectangleF s)
    {
        gg.FillRectangle(Brushes.White, s);
        using var f = new Font("Segoe UI", 18f, FontStyle.Bold);
        gg.DrawString("red\u00e9marrage\u2026", f, new SolidBrush(Color.FromArgb(90, 96, 110)), s.X + s.Width * 0.14f, s.Y + s.Height * 0.2f);
    }
    DrawVita(g, 80, 35, 260, 260, VitaScreen4);

    // big restart arrow
    using var arrow = new Pen(accent, 10f);
    arrow.StartCap = LineCap.Flat;
    arrow.EndCap = LineCap.ArrowAnchor;
    g.DrawArc(arrow, 320, 70, 200, 200, 30, 300);
    using var arrow2 = new Pen(accent, 10f);
    arrow2.EndCap = LineCap.ArrowAnchor;
    g.DrawLine(arrow2, 480, 70, 500, 100);

    Text(g, "VitaShell \u2192 plugins actifs", 14f, FontStyle.Bold, dark, 250, 300);
    Text(g, "red\u00e9marre la Vita", 13f, FontStyle.Regular, gray, 285, 320);

    bmp.Save(Path.Combine(outDir, "4.png"), ImageFormat.Png);
    Console.WriteLine("4.png done");
}

// ---------- 5.png : app CONNECTER ----------
using (var bmp = new Bitmap(W, H))
using (var g = Graphics.FromImage(bmp))
{
    g.SmoothingMode = SmoothingMode.AntiAlias;
    g.TextRenderingHint = System.Drawing.Text.TextRenderingHint.ClearTypeGridFit;
    g.Clear(bg);

    var win = new Rectangle(60, 30, 520, 300);
    DrawWindow(g, win, "VitaSync", false);

    // status
    g.FillEllipse(new SolidBrush(green), win.X + 24, win.Y + 46, 16, 16);
    Text(g, "Vita USB : CONNECT\u00c9", 14f, FontStyle.Bold, Color.FromArgb(28, 120, 60), win.X + 48, win.Y + 42);

    // button
    Pill(g, new Rectangle(win.X + 24, win.Y + 76, 220, 52), 10, accent);
    CenterText(g, "CONNECTER", 17f, FontStyle.Bold, white, win.X + 24, win.Y + 76, 220, 52);

    // mini video
    using var vid = Rounded(new Rectangle(win.Right - 260, win.Y + 76, 236, 180), 8);
    g.FillPath(new SolidBrush(Color.FromArgb(30, 34, 42)), vid);
    g.SetClip(vid);
    g.FillRectangle(new SolidBrush(Color.FromArgb(18, 116, 60)), win.Right - 250, win.Y + 120, 120, 60);
    g.FillRectangle(new SolidBrush(Color.FromArgb(120, 90, 50)), win.Right - 100, win.Y + 140, 80, 40);
    Text(g, "jeu", 15f, FontStyle.Bold, Color.Silver, win.Right - 140, win.Y + 150);
    g.ResetClip();
    Text(g, "F8 souris \u00b7 F11 infos \u00b7 F12 retour", 12f, FontStyle.Regular, gray, win.X + 24, win.Y + 300);

    bmp.Save(Path.Combine(outDir, "5.png"), ImageFormat.Png);
    Console.WriteLine("5.png done");
}

Console.WriteLine("ALL DONE");
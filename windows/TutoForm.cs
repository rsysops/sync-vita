using System;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Windows.Forms;

namespace VitaPCControl;

internal sealed class TutoForm : Form
{
    const int SlideCount = 5;

    static string FlagPath => Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData),
        "VitaSync", "tutoDone.txt");
    static string CustomDir => Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData),
        "VitaSync", "tuto");
    static string BundledDir => Path.Combine(Application.StartupPath, "assets", "tuto");

    static string Img(string name)
    {
        string custom = Path.Combine(CustomDir, name);
        if (File.Exists(custom)) return custom;
        return Path.Combine(BundledDir, name);
    }

    static string[] SlideImages(int s)
    {
        int page = s + 1;
        var list = new List<string>();
        for (int n = 1; n <= MaxImgs; n++) {
            string p = Img($"{page}-{n}.png");
            if (!File.Exists(p)) break;
            list.Add(p);
        }
        if (list.Count > 0) return list.ToArray();
        return new[] { Img($"{page}.png") };
    }

    public static bool Done
    {
        get { try { return File.Exists(FlagPath); } catch { return false; } }
    }

    public static void MarkDone()
    {
        try {
            Directory.CreateDirectory(Path.GetDirectoryName(FlagPath)!);
            File.WriteAllText(FlagPath, "1");
        } catch { }
    }

    int slide;
    const int MaxImgs = 3;
    PictureBox[] pics;
    Label[] ptags;
    Label lblTitle = new();
    Label lblBody = new();
    Button btnPrev = new();
    Button btnNext = new();
    Button btnSkip = new();
    Button btnDl = new();
    readonly Color bg = Color.FromArgb(240, 241, 245);
    readonly Color accent = Color.FromArgb(0, 102, 204);
    readonly Color dark = Color.FromArgb(30, 34, 42);
    readonly Color muted = Color.FromArgb(120, 120, 120);

    public TutoForm()
    {
        Text = "VitaSync";
        WindowState = FormWindowState.Maximized;
        FormBorderStyle = FormBorderStyle.None;
        BackColor = bg;
        DoubleBuffered = true;
        KeyPreview = true;
        Icon = Icon.ExtractAssociatedIcon(Application.ExecutablePath);

        pics = new PictureBox[MaxImgs];
        ptags = new Label[MaxImgs];
        for (int i = 0; i < MaxImgs; i++) {
            pics[i] = new PictureBox {
                SizeMode = PictureBoxSizeMode.Zoom,
                BackColor = Color.FromArgb(233, 235, 240),
                BorderStyle = BorderStyle.None,
                Visible = false
            };
            Controls.Add(pics[i]);

            ptags[i] = new Label {
                Text = (i + 1).ToString(),
                Font = new Font("Segoe UI", 11f, FontStyle.Bold),
                ForeColor = Color.White,
                BackColor = Color.FromArgb(154, 163, 175),
                Padding = new Padding(6, 2, 6, 2),
                AutoSize = true,
                Visible = false
            };
            Controls.Add(ptags[i]);
        }

        lblTitle.Font = new Font("Segoe UI", 26f, FontStyle.Bold);
        lblTitle.ForeColor = dark;
        lblTitle.AutoSize = true;
        Controls.Add(lblTitle);

        lblBody.Font = new Font("Segoe UI", 13f);
        lblBody.ForeColor = Color.FromArgb(60, 64, 72);
        lblBody.AutoSize = true;
        lblBody.MaximumSize = new Size(0, 0);
        Controls.Add(lblBody);

        void StyleBtn(Button b, Color fill)
        {
            b.FlatStyle = FlatStyle.Flat;
            b.FlatAppearance.BorderSize = 0;
            b.Font = new Font("Segoe UI", 12f, FontStyle.Bold);
            b.BackColor = fill;
            b.ForeColor = fill == accent ? Color.White : dark;
            b.Cursor = Cursors.Hand;
            b.MouseEnter += (_, __) => { b.BackColor = ControlPaint.Light(fill, 0.12f); };
            b.MouseLeave += (_, __) => { b.BackColor = fill; };
        }
        StyleBtn(btnPrev, Color.White);
        StyleBtn(btnNext, accent);
        StyleBtn(btnSkip, Color.White);
        StyleBtn(btnDl, accent);
        btnPrev.FlatAppearance.BorderSize = 1;
        btnPrev.BackColor = Color.White;
        btnPrev.Padding = new Padding(18, 8, 18, 8);
        Controls.Add(btnPrev);

        btnNext.Padding = new Padding(24, 8, 24, 8);
        Controls.Add(btnNext);

        btnSkip.FlatAppearance.BorderSize = 0;
        btnSkip.BackColor = bg;
        btnSkip.Font = new Font("Segoe UI", 11f);
        btnSkip.ForeColor = muted;
        btnSkip.Padding = new Padding(10, 6, 10, 6);
        btnSkip.MouseEnter += (_, __) => { btnSkip.BackColor = ControlPaint.Light(bg, 0.05f); };
        btnSkip.MouseLeave += (_, __) => { btnSkip.BackColor = bg; };
        btnSkip.Click += (_, __) => { MarkDone(); Close(); };
        Controls.Add(btnSkip);

        btnDl.Visible = false;
        Controls.Add(btnDl);

        btnPrev.Click += (_, __) => GoTo(slide - 1);
        btnNext.Click += (_, __) => {
            if (slide == SlideCount - 1) { MarkDone(); Close(); }
            else GoTo(slide + 1);
        };
        btnDl.Click += (_, __) => DownloadPlugin();

        GoTo(0);
    }

    protected override void OnLoad(EventArgs e)
    {
        base.OnLoad(e);
        LayoutUI();
    }

    protected override void OnResize(EventArgs e)
    {
        base.OnResize(e);
        if (IsHandleCreated) LayoutUI();
    }

    void GoTo(int i)
    {
        slide = Math.Clamp(i, 0, SlideCount - 1);
        lblTitle.Text = L.T($"tuto.{slide + 1}title");
        lblBody.Text = L.T($"tuto.{slide + 1}body");
        try {
            var paths = SlideImages(slide);
            int count = Math.Min(paths.Length, MaxImgs);
            foreach (var img in pics) img.Image?.Dispose();
            for (int n = 0; n < MaxImgs; n++) {
                bool on = n < count;
                pics[n].Visible = on;
                ptags[n].Visible = on;
                pics[n].Image = (on && File.Exists(paths[n])) ? new Bitmap(paths[n]) : null;
            }
        } catch {
            foreach (var img in pics) img.Image = null;
        }
        btnNext.Text = (slide == SlideCount - 1) ? L.T("tuto.finish") : L.T("tuto.next");
        btnPrev.Text = L.T("tuto.prev");
        btnPrev.Enabled = slide > 0;
        btnSkip.Text = L.T("tuto.skip");
        btnDl.Text = L.T("tuto.dl");
        btnDl.Visible = slide == 0;
        btnDl.Enabled = true;
        LayoutUI();
        Invalidate();
    }

    void LayoutUI()
    {
        var r = ClientRectangle;
        int W = r.Width, H = r.Height;

        lblTitle.Left = (int)(W * 0.52f);
        lblTitle.Top = (int)(H * 0.16f);

        lblBody.Left = (int)(W * 0.52f);
        lblBody.Top = (int)(H * 0.16f) + lblTitle.Height + 26;
        lblBody.MaximumSize = new Size((int)(W * 0.40f), 0);

        int zoneX = (int)(W * 0.07f), zoneY = (int)(H * 0.14f);
        int zoneW = (int)(W * 0.40f), zoneH = (int)(H * 0.56f);
        int zoneR = (int)(W * 0.07f) + zoneW;
        int visible = 0;
        for (int n = 0; n < MaxImgs; n++)
            if (pics[n].Visible) visible++;
        int gap = (int)(W * 0.012f);
        if (visible > 1) {
            int w = (zoneW - gap * (visible - 1)) / visible;
            int x = zoneX;
            for (int n = 0; n < MaxImgs && x < zoneR; n++) {
                if (!pics[n].Visible) continue;
                pics[n].Left = x; pics[n].Top = zoneY; pics[n].Width = w; pics[n].Height = zoneH;
                ptags[n].Left = x + 8; ptags[n].Top = zoneY + 8;
                x += w + gap;
            }
        } else {
            pics[0].Left = zoneX; pics[0].Top = zoneY; pics[0].Width = zoneW; pics[0].Height = zoneH;
            ptags[0].Left = zoneX + 8; ptags[0].Top = zoneY + 8;
        }

        btnDl.Left = lblBody.Left;
        btnDl.Top = lblBody.Bottom + 28;
        btnDl.Height = 44;
        btnDl.Width = (int)(W * 0.15f);

        int skipW = TextRenderer.MeasureText(btnSkip.Text, btnSkip.Font).Width + btnSkip.Padding.Horizontal + 24;
        btnSkip.Left = W - skipW - (int)(W * 0.02f);
        btnSkip.Top = (int)(H * 0.03f);
        btnSkip.Width = skipW;
        btnSkip.Height = 34;

        btnPrev.AutoSize = false;
        btnPrev.Width = (int)(W * 0.09f);
        btnPrev.Height = 44;
        btnPrev.Left = (int)(W * 0.40f);
        btnPrev.Top = (int)(H * 0.86f);

        btnNext.AutoSize = false;
        btnNext.Width = (int)(W * 0.09f);
        btnNext.Height = 44;
        btnNext.Left = (int)(W * 0.51f);
        btnNext.Top = (int)(H * 0.86f);
    }

    protected override void OnPaint(PaintEventArgs e)
    {
        base.OnPaint(e);
        var g = e.Graphics;
        g.SmoothingMode = System.Drawing.Drawing2D.SmoothingMode.AntiAlias;

        var r = ClientRectangle;
        int W = r.Width, H = r.Height;

        /* Titre du tutoriel */
        using var titleFont = new Font("Segoe UI", Math.Max(18, (int)(H * 0.030f)), FontStyle.Bold);
        var tt = L.T("tuto.title");
        var ts = g.MeasureString(tt, titleFont);
        g.DrawString(tt, titleFont, new SolidBrush(dark),
            (W - ts.Width) / 2f, H * 0.05f);

        /* Numéro de page en haut à gauche */
        using var pageFont = new Font("Segoe UI", Math.Max(11, (int)(H * 0.016f)), FontStyle.Bold);
        var ps = g.MeasureString($"{(slide + 1)} / {SlideCount}", pageFont);
        g.DrawString($"{slide + 1} / {SlideCount}", pageFont, new SolidBrush(muted),
            (int)(W * 0.02f), (int)(H * 0.03f));

        /* Puces de progression */
        int dotGap = (int)(W * 0.014f);
        int dotD = Math.Max(8, (int)(H * 0.012f));
        int dotsW = SlideCount * dotD + (SlideCount - 1) * dotGap;
        int dx = (W - dotsW) / 2;
        int dy = (int)(H * 0.79f);
        for (int i = 0; i < SlideCount; i++) {
            var rc = new Rectangle(dx + i * (dotD + dotGap), dy, dotD, dotD);
            if (i == slide)
                g.FillEllipse(new SolidBrush(accent), rc);
            else {
                g.FillEllipse(Brushes.White, rc);
                using var pen = new Pen(Color.FromArgb(180, 186, 196), 2f);
                g.DrawEllipse(pen, rc);
            }
        }
    }

    void DownloadPlugin()
    {
        try {
            string src = Path.Combine(Application.StartupPath, "VitaSync.skprx");
            if (!File.Exists(src)) {
                MessageBox.Show(L.T("tuto.dlMissing"), "VitaSync",
                    MessageBoxButtons.OK, MessageBoxIcon.Information);
                return;
            }
            string dest = Path.Combine(
                Environment.GetFolderPath(Environment.SpecialFolder.UserProfile),
                "Downloads", "VitaSync.skprx");
            File.Copy(src, dest, true);
            MessageBox.Show(L.T("tuto.dlOpen", dest), "VitaSync",
                MessageBoxButtons.OK, MessageBoxIcon.Information);
            try { Process.Start("explorer.exe", $"/select,\"{dest}\""); } catch { }
            btnDl.Text = L.T("tuto.dlDone");
            btnDl.Enabled = false;
        } catch (Exception ex) {
            MessageBox.Show(ex.Message, "VitaSync", MessageBoxButtons.OK, MessageBoxIcon.Warning);
        }
    }

    protected override void OnKeyDown(KeyEventArgs e)
    {
        base.OnKeyDown(e);
        switch (e.KeyCode) {
            case Keys.Left:   GoTo(slide - 1); break;
            case Keys.Right:
            case Keys.Enter:  if (slide == SlideCount - 1) { MarkDone(); Close(); } else GoTo(slide + 1); break;
            case Keys.Escape: MarkDone(); Close(); break;
        }
    }
}
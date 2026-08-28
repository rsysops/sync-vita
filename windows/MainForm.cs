using System;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
using System.Windows.Forms;
using System.Linq;
using System.Collections.Generic;
using HidSharp;

namespace VitaPCControl;

public class MainForm : Form
{
    class HUDOverlay : Panel
    {
        Func<Image?>? getBackdrop;
        public HUDOverlay(Func<Image?> backdrop) {
            getBackdrop = backdrop;
            DoubleBuffered = true;
            SetStyle(ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.UserPaint, true);
        }
        protected override void OnPaintBackground(PaintEventArgs e) {
            base.OnPaintBackground(e);
            var g = e.Graphics;
            var img = getBackdrop?.Invoke();
            if (img != null) {
                float sc = Math.Max((float)Width / img.Width, (float)Height / img.Height);
                int w = (int)(img.Width * sc), h = (int)(img.Height * sc);
                g.DrawImage(img, (Width - w) / 2, (Height - h) / 2, w, h);
            }
            g.FillRectangle(new SolidBrush(Color.FromArgb(195, 0, 0, 0)), 0, 0, Width, Height);
        }
    }
    class VideoSurface : Control
    {
        Bitmap? current;
        public VideoSurface() {
            DoubleBuffered = true;
            SetStyle(ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.UserPaint, true);
        }
        public Image? Image => current;
        public void Publish(Bitmap? bmp) {
            current = bmp;
            Invalidate();
        }
        protected override void OnPaint(PaintEventArgs e) {
            base.OnPaint(e);
            var img = current;
            if (img == null) return;
            var g = e.Graphics;
            float sc = Math.Min((float)Width / img.Width, (float)Height / img.Height);
            int w = (int)(img.Width * sc), h = (int)(img.Height * sc);
            g.InterpolationMode = InterpolationMode.NearestNeighbor;
            g.CompositingMode = CompositingMode.SourceCopy;
            g.PixelOffsetMode = PixelOffsetMode.Half;
            g.DrawImage(img, (Width - w) / 2, (Height - h) / 2, w, h);
        }
    }
    HidDevice? device;
    HidStream? stream;
    Label debug = new();
    Label diag = new();
    System.Threading.Thread? reader;
    TrackBar sensitivity = new();
    System.Threading.Thread? sendThread;
    volatile bool sendRunning;
    int sensCache = 700;
    readonly byte[] rawBuf = new byte[64];
    GCHandle rawHandle;
    Thread? rawThread;
    volatile bool rawThreadRun;
    IntPtr rawWnd;
    WndProcDelegate? rawWndProc;
    Point cursorCenter = Point.Empty;
    readonly object sync = new();
    ushort buttons;
    byte lx=128, ly=128, rx=128, ry=128;
    int mouseDx, mouseDy;
    bool touchDown;
    int touchX, touchY;
    int kbEvCount, touchEvCount;
    bool escDown;
    DateTime escSince = DateTime.MinValue;
    bool lastExitCmd;
    byte lastFlags; int lastTx, lastTy;
    ushort lastButtons; byte lastLx=128, lastLy=128, lastRx=128, lastRy=128;
    DateTime lastSendUtc = DateTime.MinValue;
    bool running;
    KeyboardHook? keyboard;
    MouseHook? mouse;
    Windows.Media.Capture.MediaCapture? mediaCapture;
    Windows.Media.Capture.Frames.MediaFrameReader? frameReader;
    byte[]? frameSrcBuf;
    volatile bool framePending;
    volatile int bmpGen;
    byte[] g16 = new byte[16];
    byte[] rowG = new byte[8];
    Bitmap? videoBmp0, videoBmp1;
    int vbpIdx;
    VideoSurface video = new();
    Label camStat = new();
    System.Windows.Forms.TextBox audioBox = new();
    int frameCount;
    DateTime lastFpsCheck = DateTime.MinValue;
    bool capturing;
    readonly string settingsPath = Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData),
        "VitaSync", "sensitivity.txt");
    readonly string keymapPath = Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData),
        "VitaSync", "keymap.txt");

    const ushort TRIANGLE=0x1000, CIRCLE=0x2000, CROSS=0x4000, SQUARE=0x8000;
    const ushort LTRIGGER=0x0100, RTRIGGER=0x0200;
    const ushort SELECT=0x0001, START=0x0008;
    const ushort UP=0x0010, RIGHT=0x0020, DOWN=0x0040, LEFT=0x0080;

    /* ── Screens ── */
    Panel hubPanel = new();
    Panel videoPanel = new();
    Panel overlayPanel;
    FlowLayoutPanel ovFlow = new();
    Panel settingsOverlay = new();
    Panel keymapOverlay = new();
    Panel helpOverlay = new();
    Panel faqOverlay = new();
    Panel settingsMenu = new();
    FlowLayoutPanel keymapFlow = new();
    Label keymapWarn = new();
    Dictionary<ushort, Button> keymapBtns = new();
    Button keymapSettingsBtn = new();
    readonly Color keymapFg = Color.FromArgb(40, 40, 40);
    bool overlayVisible;
    bool closeHover;
    bool settingsHover;
    Label hubStatus = new();
    bool inVideoMode;

    /* Hub buttons */
    Button hubConnect = new();
    Button hubDisconnect = new();

    /* Language-text controls (refreshed by RefreshAllText) */
    Label infoLabel = new();
    Label settingsBtn = new();
    Label sensLabel = new();
    Label hintLbl = new();
    Label settingsTitle = new();
    Label langLabel = new();
    Button closeSettings = new();
    Label keymapTitle = new();
    Label keymapHint = new();
    Button keymapReset = new();
    Button keymapBack = new();
    Button keymapSave = new();
    Label helpTitle = new();
    Label helpBody = new();
    Button helpClose = new();
    Label faqTitle = new();
    Label faqBody = new();
    Button faqClose = new();
    Label vidHint = new();
    Button ovDisconnect = new();
    Label ovZqsdLbl = new();
    Dictionary<ushort, Label> ovKeymapNameLbl = new();

    /* Overlay labels */
    Label overlayStatus = new();
    Label debugLabel = new();
    TextBox overlayDiag = new();
    TextBox overlayAudio = new();
    Color ovOkColor = Color.FromArgb(90, 210, 130);
    TrackBar? ovSensitivity;
    TableLayoutPanel? ovKeymapGrid;
    Dictionary<ushort, Label> ovKeymapKeyLbl = new();
    Bitmap? ovBackdrop;

    public MainForm()
    {
        timeBeginPeriod(1);
        btnKeys = new Dictionary<ushort, int>(DefaultBtnKeys);
        LoadKeyMap();
        RebuildKeyMap();
        Text = "VitaSync";
        Icon = Icon.ExtractAssociatedIcon(Application.ExecutablePath);
        KeyPreview = true;
        WindowState = FormWindowState.Maximized;
        FormBorderStyle = FormBorderStyle.None;
        BackColor = Color.FromArgb(240, 241, 245);
        DoubleBuffered = true;

        var screen = Screen.PrimaryScreen!.WorkingArea;
        int W = screen.Width, H = screen.Height;
        var font = new Font("Segoe UI", 10f);
        var fontBold = new Font("Segoe UI", 10f, FontStyle.Bold);
        var accent = Color.FromArgb(0, 102, 204);
        var fg = Color.FromArgb(40, 40, 40);
        var muted = Color.FromArgb(120, 120, 120);
        var cardBg = Color.White;

        void StyleBtn(Button b, Color bg, bool large = false) {
            b.FlatStyle = FlatStyle.Flat;
            b.FlatAppearance.BorderSize = 0;
            b.BackColor = bg;
            b.ForeColor = Color.White;
            b.Font = fontBold;
            b.Cursor = Cursors.Hand;
            b.Height = large ? (int)(H * 0.065) : (int)(H * 0.045);
            b.Width = large ? (int)(W * 0.18) : (int)(W * 0.12);
            b.Text = b.Text.ToUpper();
            b.FlatAppearance.MouseOverBackColor = ControlPaint.Light(bg, 0.15f);
        }

        /* ══════════════════════════════════════════
           HUB SCREEN
           ══════════════════════════════════════════ */
        hubPanel.Dock = DockStyle.Fill;
        hubPanel.BackColor = Color.FromArgb(240, 241, 245);
        Controls.Add(hubPanel);

        /* ── Close button (X) top-right ── */
        int btnSz = (int)(H * 0.05f);
        int btnMg = (int)(W * 0.015f);
        var hubClose = new Panel { Width = btnSz, Height = btnSz };
        hubClose.Left = W - btnSz - btnMg;
        hubClose.Top = btnMg;
        hubClose.Cursor = Cursors.Hand;
        bool hubCloseHover = false;
        hubClose.Paint += (_, e) => {
            e.Graphics.SmoothingMode = SmoothingMode.AntiAlias;
            if (hubCloseHover) {
                using var bg = new SolidBrush(Color.FromArgb(230, 230, 235));
                e.Graphics.FillEllipse(bg, 0, 0, btnSz, btnSz);
            }
            using var xFont = new Font("Segoe UI", btnSz * 0.45f);
            var sz = e.Graphics.MeasureString("\u2715", xFont);
            e.Graphics.DrawString("\u2715", xFont, hubCloseHover ? Brushes.Black : Brushes.Gray,
                (btnSz - sz.Width) / 2f, (btnSz - sz.Height) / 2f);
        };
        hubClose.MouseEnter += (_,__) => { hubCloseHover = true; hubClose.Invalidate(); };
        hubClose.MouseLeave += (_,__) => { hubCloseHover = false; hubClose.Invalidate(); };
        hubClose.Click += (_,__) => Application.Exit();
        hubPanel.Controls.Add(hubClose);

        /* ── Title ── */
        var titleFont = new Font("Segoe UI", Math.Max(22, (int)(H * 0.038)), FontStyle.Bold);
        var titleLbl = new Label { Text="VitaSync", Font=titleFont, ForeColor=fg,
            AutoSize=true, TextAlign=ContentAlignment.MiddleCenter };
        titleLbl.Left = (W - titleLbl.PreferredWidth) / 2;
        titleLbl.Top = (int)(H * 0.12);
        hubPanel.Controls.Add(titleLbl);

        /* ── Status ── */
        hubStatus.Font = new Font("Segoe UI", 11f);
        hubStatus.ForeColor = muted;
        hubStatus.Text = L.T("status.waiting");
        hubStatus.AutoSize = true;
        hubStatus.Left = (W - hubStatus.PreferredWidth) / 2;
        hubStatus.Top = (int)(H * 0.20);
        hubPanel.Controls.Add(hubStatus);

        /* ── USB indicator dot ── */
        var usbDot = new Panel { Width=12, Height=12, BackColor=Color.FromArgb(200,200,200) };
        usbDot.Left = hubStatus.Left - 20;
        usbDot.Top = hubStatus.Top + 5;
        usbDot.Paint += (_,e) => {
            e.Graphics.SmoothingMode = SmoothingMode.AntiAlias;
            var col = running ? Color.FromArgb(76, 175, 80) : Color.FromArgb(200, 200, 200);
            using var br = new SolidBrush(col);
            e.Graphics.FillEllipse(br, 0, 0, 12, 12);
        };
        hubPanel.Controls.Add(usbDot);
        hubPanel.Controls.Add(hubStatus);

        /* ── Info card ── */
        int infoW = (int)(W * 0.35);
        int infoH = (int)(H * 0.32);
        var infoCard = new Panel {
            Left = (W - infoW) / 2, Top = (int)(H * 0.28),
            Width = infoW, Height = infoH, BackColor = cardBg
        };
        infoCard.Paint += (_,e) => {
            using var pen = new Pen(Color.FromArgb(220,220,225));
            e.Graphics.DrawRectangle(pen, 0, 0, infoCard.Width-1, infoCard.Height-1);
        };
        hubPanel.Controls.Add(infoCard);

        infoLabel = new Label {
            Text = L.T("hub.info"),
            Font = new Font("Segoe UI", 9.5f),
            ForeColor = Color.FromArgb(80, 80, 80),
            Left = 16, Top = 16,
            Width = infoW - 32, Height = infoH - 32,
            AutoSize = false
        };
        infoCard.Controls.Add(infoLabel);

        /* ── Settings button (top-left) with hover dropdown menu ── */
        settingsBtn = new Label {
            Text = "\u2699 " + L.T("settings.title"),
            Font = new Font("Segoe UI", 10f),
            ForeColor = muted,
            AutoSize = true, Cursor = Cursors.Hand
        };
        settingsBtn.Left = 0;
        settingsBtn.Top = 0;

        var settingsZone = new Panel { BackColor = Color.Transparent };
        settingsZone.Left = btnMg;
        settingsZone.Top = btnMg;
        settingsZone.Width = 240;
        settingsZone.Height = settingsBtn.Height + 2 + 4 * 52 + 8 + 4;
        hubPanel.Controls.Add(settingsZone);

        settingsZone.Controls.Add(settingsBtn);

        settingsMenu.Visible = false;
        settingsMenu.BackColor = Color.FromArgb(250, 250, 252);
        settingsMenu.Width = 230;
        settingsMenu.Height = 4 * 52 + 8;
        settingsMenu.Padding = new Padding(4);
        settingsMenu.Left = 0;
        settingsMenu.Top = settingsBtn.Bottom + 2;
        settingsZone.Controls.Add(settingsMenu);
        settingsZone.Height = settingsMenu.Bottom + 4;
        RebuildSettingsMenu();

        var menuTimer = new System.Windows.Forms.Timer { Interval = 120 };
        menuTimer.Tick += (_,__) => {
            if (CursorInside(settingsZone)) {
                settingsMenu.Visible = true;
                settingsMenu.BringToFront();
            } else {
                settingsMenu.Visible = false;
                menuTimer.Stop();
            }
        };
        settingsBtn.MouseEnter += (_,__) => { settingsBtn.ForeColor = accent; menuTimer.Stop(); settingsMenu.Visible = true; settingsMenu.BringToFront(); };
        settingsBtn.MouseLeave += (_,__) => { settingsBtn.ForeColor = muted; };
        settingsBtn.Click += (_,__) => { };
        settingsZone.MouseLeave += (_,__) => menuTimer.Start();
        settingsZone.MouseEnter += (_,__) => menuTimer.Stop();

        /* ── Connect button (centered bottom) ── */
        hubConnect.Text = L.T("btn.connect");
        StyleBtn(hubConnect, accent, true);
        hubConnect.Left = (W - hubConnect.Width) / 2;
        hubConnect.Top = (int)(H * 0.73);
        hubConnect.Click += (_,__) => ConnectDevice();
        hubPanel.Controls.Add(hubConnect);

        /* ── Disconnect button (centered bottom, initially hidden) ── */
        hubDisconnect.Text = L.T("btn.disconnect");
        StyleBtn(hubDisconnect, Color.FromArgb(200, 50, 50), true);
        hubDisconnect.Left = (W - hubDisconnect.Width) / 2;
        hubDisconnect.Top = (int)(H * 0.73);
        hubDisconnect.Click += (_,__) => { StopControl(); RefreshHubStatus(); };
        hubDisconnect.Visible = false;
        hubPanel.Controls.Add(hubDisconnect);

        /* ── Hint ── */
        hintLbl = new Label {
            Text = L.T("hub.hint"),
            Font = new Font("Segoe UI", 8.5f),
            ForeColor = Color.FromArgb(180, 180, 185),
            AutoSize = true
        };
        hintLbl.Left = (W - hintLbl.PreferredWidth) / 2;
        hintLbl.Top = (int)(H * 0.82);
        hubPanel.Controls.Add(hintLbl);

        /* ── Sensitivity (on hub, below hint) ── */
        sensLabel = new Label { Text=L.T("sensitivity"), Font=font, ForeColor=muted,
            AutoSize=true };
        sensLabel.Left = (W - (int)(W * 0.25) - sensLabel.PreferredWidth) / 2;
        sensLabel.Top = (int)(H * 0.86);
        hubPanel.Controls.Add(sensLabel);
        sensitivity.Minimum=1; sensitivity.Maximum=50000; sensitivity.Value=700; sensitivity.TabStop=false;
#if !APPTEST
        try {
            if (int.TryParse(File.ReadAllText(settingsPath), out int v))
                sensitivity.Value = Math.Clamp(v, sensitivity.Minimum, sensitivity.Maximum);
        } catch { }
#endif
        sensitivity.ValueChanged += (_,__) => { sensCache = sensitivity.Value; SaveSensitivity(); };
        sensitivity.Left = sensLabel.Left + sensLabel.PreferredWidth + 10;
        sensitivity.Top = sensLabel.Top - 3;
        sensitivity.Width = (int)(W * 0.25);
        sensitivity.TickFrequency = 500;
        hubPanel.Controls.Add(sensitivity);

        /* ══════════════════════════════════════════
           SETTINGS OVERLAY
           ══════════════════════════════════════════ */
        settingsOverlay.Visible = false;
        settingsOverlay.BackColor = Color.FromArgb(200, 200, 210);
        settingsOverlay.Dock = DockStyle.Fill;
        Controls.Add(settingsOverlay);

        settingsTitle = new Label {
            Text = L.T("settings.title"),
            Font = new Font("Segoe UI", 16f, FontStyle.Bold),
            ForeColor = fg, AutoSize = true
        };
        settingsTitle.Left = (W - settingsTitle.PreferredWidth) / 2;
        settingsTitle.Top = (int)(H * 0.2);
        settingsOverlay.Controls.Add(settingsTitle);

        langLabel = new Label {
            Text = L.T("settings.lang"),
            Font = new Font("Segoe UI", 12f),
            ForeColor = fg, AutoSize = true
        };
        langLabel.Left = (W - langLabel.PreferredWidth) / 2;
        langLabel.Top = (int)(H * 0.32);
        settingsOverlay.Controls.Add(langLabel);

        /* Language buttons */
        string[] langCodes = {"fr", "en", "es"};
        string[] langLabels = {"FR", "EN", "ES"};
        Color[] langColors = {
            Color.FromArgb(0, 35, 149),
            Color.FromArgb(0, 36, 125),
            Color.FromArgb(187, 0, 0)
        };
        int langBtnW = 100, langBtnH = 50, langGap = 20;
        int langTotal = langBtnW * 3 + langGap * 2;
        int langStartX = (W - langTotal) / 2;
        int langY = (int)(H * 0.38);

        for (int i = 0; i < 3; i++) {
            int idx = i;
            var lb = new Button {
                Text = langLabels[i],
                Font = new Font("Segoe UI", 14f, FontStyle.Bold),
                ForeColor = Color.White,
                BackColor = langColors[i],
                FlatStyle = FlatStyle.Flat,
                Width = langBtnW, Height = langBtnH,
                Left = langStartX + idx * (langBtnW + langGap),
                Top = langY,
                Cursor = Cursors.Hand,
                Tag = langCodes[idx]
            };
            lb.FlatAppearance.BorderSize = L.Lang == langCodes[idx] ? 3 : 0;
            lb.FlatAppearance.BorderColor = accent;
            lb.Click += (s,__) => {
                string code = (string)((Button)s!).Tag!;
                L.Set(code);
                /* Update all UI text */
                RefreshAllText();
                /* Refresh lang button borders */
                foreach (Control c in settingsOverlay.Controls) {
                    if (c is Button b2 && b2.Tag is string sc)
                        b2.FlatAppearance.BorderSize = sc == L.Lang ? 3 : 0;
                }
            };
            settingsOverlay.Controls.Add(lb);
        }

        /* Close settings button */
        closeSettings = new Button {
            Text = L.T("settings.close"),
            Font = fontBold, ForeColor = Color.White,
            BackColor = accent,
            FlatStyle = FlatStyle.Flat, Width = 160, Height = 45,
            Cursor = Cursors.Hand
        };
        closeSettings.FlatAppearance.BorderSize = 0;
        closeSettings.Left = (W - closeSettings.Width) / 2;
        closeSettings.Top = (int)(H * 0.55);
        closeSettings.Click += (_,__) => ToggleSettings();
        settingsOverlay.Controls.Add(closeSettings);

        /* ── Keymap settings button ── */
        keymapSettingsBtn = new Button {
            Text = L.T("settings.keymap"),
            Font = fontBold, ForeColor = Color.White,
            BackColor = accent,
            FlatStyle = FlatStyle.Flat, Width = 200, Height = 45,
            Cursor = Cursors.Hand
        };
        keymapSettingsBtn.FlatAppearance.BorderSize = 0;
        keymapSettingsBtn.Left = (W - keymapSettingsBtn.Width) / 2;
        keymapSettingsBtn.Top = (int)(H * 0.48);
        keymapSettingsBtn.Click += (_,__) => { keymapOverlay.Visible = true; keymapOverlay.BringToFront(); RefreshKeymapRows(); };
        settingsOverlay.Controls.Add(keymapSettingsBtn);

        /* ══════════════════════════════════════════
           KEYMAP OVERLAY
           ══════════════════════════════════════════ */
        keymapOverlay.Visible = false;
        keymapOverlay.BackColor = Color.FromArgb(205, 205, 215);
        keymapOverlay.Dock = DockStyle.Fill;
        Controls.Add(keymapOverlay);

        keymapTitle = new Label {
            Text = L.T("keymap.title"),
            Font = new Font("Segoe UI", 16f, FontStyle.Bold),
            ForeColor = fg, AutoSize = true
        };
        keymapTitle.Left = (W - keymapTitle.PreferredWidth) / 2;
        keymapTitle.Top = (int)(H * 0.04);
        keymapOverlay.Controls.Add(keymapTitle);

        keymapWarn.Text = "";
        keymapWarn.Font = new Font("Segoe UI", 10f);
        keymapWarn.AutoSize = true;
        keymapWarn.Left = (W - 500) / 2;
        keymapWarn.Top = (int)(H * 0.09);
        keymapWarn.Width = 500;
        keymapOverlay.Controls.Add(keymapWarn);

        keymapHint = new Label {
            Text = L.T("keymap.hint"),
            Font = new Font("Segoe UI", 10f),
            ForeColor = muted, AutoSize = true
        };
        keymapHint.Left = (W - keymapHint.PreferredWidth) / 2;
        keymapHint.Top = (int)(H * 0.13);
        keymapOverlay.Controls.Add(keymapHint);

        keymapFlow = new FlowLayoutPanel {
            FlowDirection = FlowDirection.TopDown,
            WrapContents = false,
            AutoScroll = true,
            BackColor = Color.FromArgb(250, 250, 252),
            Left = (int)(W * 0.3), Top = (int)(H * 0.18),
            Width = (int)(W * 0.4), Height = (int)(H * 0.55),
            Padding = new Padding(10)
        };
        keymapOverlay.Controls.Add(keymapFlow);

        keymapReset = new Button {
            Text = L.T("keymap.reset"),
            Font = fontBold, ForeColor = Color.White,
            BackColor = Color.FromArgb(200, 50, 50),
            FlatStyle = FlatStyle.Flat, Width = 170, Height = 42,
            Cursor = Cursors.Hand
        };
        keymapReset.FlatAppearance.BorderSize = 0;
        keymapReset.Left = (W - 550) / 2;
        keymapReset.Top = (int)(H * 0.78);
        keymapReset.Click += (_,__) => { keymapWarn.Text = ""; btnKeys = new Dictionary<ushort, int>(DefaultBtnKeys); RebuildKeyMap(); EndCapture(); RefreshKeymapRows(); };
        keymapOverlay.Controls.Add(keymapReset);

        keymapBack = new Button {
            Text = L.T("keymap.back"),
            Font = fontBold, ForeColor = Color.White,
            BackColor = accent,
            FlatStyle = FlatStyle.Flat, Width = 170, Height = 42,
            Cursor = Cursors.Hand
        };
        keymapBack.FlatAppearance.BorderSize = 0;
        keymapBack.Left = (W - 550) / 2 + 380;
        keymapBack.Top = (int)(H * 0.78);
        keymapBack.Click += (_,__) => { EndCapture(); keymapOverlay.Visible = false; };
        keymapOverlay.Controls.Add(keymapBack);

        keymapSave = new Button {
            Text = L.T("keymap.save"),
            Font = fontBold, ForeColor = Color.White,
            BackColor = Color.FromArgb(0, 160, 60),
            FlatStyle = FlatStyle.Flat, Width = 170, Height = 42,
            Cursor = Cursors.Hand
        };
        keymapSave.FlatAppearance.BorderSize = 0;
        keymapSave.Left = (W - 550) / 2 + 190;
        keymapSave.Top = (int)(H * 0.78);
        keymapSave.Click += (_,__) => {
            SaveKeyMap();
            EndCapture();
            keymapOverlay.Visible = false;
        };
        keymapOverlay.Controls.Add(keymapSave);

        BuildKeymapRows();

        /* ══════════════════════════════════════════
           HELP OVERLAY
           ══════════════════════════════════════════ */
        helpOverlay.Visible = false;
        helpOverlay.BackColor = Color.FromArgb(200, 200, 210);
        helpOverlay.Dock = DockStyle.Fill;
        Controls.Add(helpOverlay);

        helpTitle = new Label {
            Text = L.T("help.title"),
            Font = new Font("Segoe UI", 16f, FontStyle.Bold),
            ForeColor = fg, AutoSize = true
        };
        helpTitle.Left = (W - helpTitle.PreferredWidth) / 2;
        helpTitle.Top = (int)(H * 0.08);
        helpOverlay.Controls.Add(helpTitle);

        var helpScroll = new Panel {
            Left = (int)(W * 0.16), Top = (int)(H * 0.16),
            Width = (int)(W * 0.68), Height = (int)(H * 0.58),
            AutoScroll = true, BackColor = Color.FromArgb(190, 190, 200)
        };
        helpOverlay.Controls.Add(helpScroll);

        helpBody = new Label {
            Text = L.T("help.content"),
            Font = new Font("Segoe UI", 11f),
            ForeColor = fg,
            Dock = DockStyle.Top, AutoSize = true,
            Padding = new Padding(12)
        };
        helpScroll.Controls.Add(helpBody);

        helpClose = new Button {
            Text = L.T("settings.close"),
            Font = fontBold, ForeColor = Color.White,
            BackColor = accent,
            FlatStyle = FlatStyle.Flat, Width = 160, Height = 45,
            Cursor = Cursors.Hand
        };
        helpClose.FlatAppearance.BorderSize = 0;
        helpClose.Left = (W - helpClose.Width) / 2;
        helpClose.Top = (int)(H * 0.82);
        helpClose.Click += (_,__) => helpOverlay.Visible = false;
        helpOverlay.Controls.Add(helpClose);

        /* ══════════════════════════════════════════
           FAQ OVERLAY
           ══════════════════════════════════════════ */
        faqOverlay.Visible = false;
        faqOverlay.BackColor = Color.FromArgb(200, 200, 210);
        faqOverlay.Dock = DockStyle.Fill;
        Controls.Add(faqOverlay);

        faqTitle = new Label {
            Text = L.T("faq.title"),
            Font = new Font("Segoe UI", 16f, FontStyle.Bold),
            ForeColor = fg, AutoSize = true
        };
        faqTitle.Left = (W - faqTitle.PreferredWidth) / 2;
        faqTitle.Top = (int)(H * 0.08);
        faqOverlay.Controls.Add(faqTitle);

        var faqScroll = new Panel {
            Left = (int)(W * 0.16), Top = (int)(H * 0.16),
            Width = (int)(W * 0.68), Height = (int)(H * 0.58),
            AutoScroll = true, BackColor = Color.FromArgb(190, 190, 200)
        };
        faqOverlay.Controls.Add(faqScroll);

        faqBody = new Label {
            Text = L.T("faq.content"),
            Font = new Font("Segoe UI", 11f),
            ForeColor = fg,
            Dock = DockStyle.Top, AutoSize = true,
            Padding = new Padding(12)
        };
        faqScroll.Controls.Add(faqBody);

        faqClose = new Button {
            Text = L.T("settings.close"),
            Font = fontBold, ForeColor = Color.White,
            BackColor = accent,
            FlatStyle = FlatStyle.Flat, Width = 160, Height = 45,
            Cursor = Cursors.Hand
        };
        faqClose.FlatAppearance.BorderSize = 0;
        faqClose.Left = (W - faqClose.Width) / 2;
        faqClose.Top = (int)(H * 0.82);
        faqClose.Click += (_,__) => faqOverlay.Visible = false;
        faqOverlay.Controls.Add(faqClose);

        /* ══════════════════════════════════════════
           VIDEO SCREEN
           ══════════════════════════════════════════ */
        videoPanel.Dock = DockStyle.Fill;
        videoPanel.BackColor = Color.Black;
        videoPanel.Visible = false;
        Controls.Add(videoPanel);

        video.Dock = DockStyle.Fill;
        video.Resize += (_,__) => UpdateCursorCenter();
        video.BackColor = Color.Black;
        video.MouseDown += VideoTouch;
        video.MouseMove += VideoTouch;
        video.MouseUp += (_,__) => UpdateTouch(false,0,0);
        video.MouseLeave += (_,__) => UpdateTouch(false,0,0);
        videoPanel.Controls.Add(video);

        camStat.Font = new Font("Segoe UI", 16f, FontStyle.Bold);
        camStat.ForeColor = Color.White;
        camStat.BackColor = Color.FromArgb(200, 0, 0, 0);
        camStat.Text = "LOADING...";
        camStat.AutoSize = true;
        camStat.Left = 10; camStat.Top = 10;
        camStat.BringToFront();
        videoPanel.Controls.Add(camStat);

        /* Video bottom hint */
        vidHint = new Label {
            Text = L.T("video.hint"),
            Font = new Font("Segoe UI", 9f),
            ForeColor = Color.FromArgb(120, 120, 120),
            AutoSize = true,
            Anchor = AnchorStyles.Bottom | AnchorStyles.Left
        };
        vidHint.Left = 10;
        vidHint.Top = H - 30;
        videoPanel.Controls.Add(vidHint);

        /* ── Overlay panel (F11) — dark HUD ── */
        int ovW = Math.Max(300, Math.Min(430, (int)(W * 0.24)));
        var ovText = Color.FromArgb(235, 235, 240);
        var ovDim = Color.FromArgb(150, 155, 165);
        var ovGreen = Color.FromArgb(90, 210, 130);
        var ovSep = Color.FromArgb(45, 50, 60);
        ovOkColor = ovGreen;

        overlayPanel = new HUDOverlay(() => ovBackdrop);
        overlayPanel.Width = ovW;
        overlayPanel.Dock = DockStyle.Left;
        overlayPanel.Visible = false;
        overlayPanel.BackColor = Color.Black;
        overlayPanel.BackgroundImageLayout = ImageLayout.None;
        videoPanel.Controls.Add(overlayPanel);
        overlayPanel.BringToFront();

        overlayPanel.Controls.Add(new Panel {
            Dock = DockStyle.Left, Width = 4, BackColor = accent
        });

        ovFlow = new FlowLayoutPanel {
            Dock = DockStyle.Fill,
            AutoScroll = true,
            FlowDirection = FlowDirection.TopDown,
            WrapContents = false,
            BackColor = Color.Transparent,
            Padding = new Padding(16)
        };
        overlayPanel.Controls.Add(ovFlow);

        void OvHead(string s) => ovFlow.Controls.Add(new Label {
            Text = s.ToUpper(),
            Font = new Font("Segoe UI", 8.5f, FontStyle.Bold),
            ForeColor = ovDim,
            Width = ovW - 40, Height = 24,
            Margin = new Padding(0, 12, 0, 0)
        });
        void OvSep() => ovFlow.Controls.Add(new Panel {
            Width = ovW - 40, Height = 1, BackColor = ovSep,
            Margin = new Padding(0, 6, 0, 10)
        });

        /* Overlay: title */
        ovFlow.Controls.Add(new Label {
            Text = "VITASYNC",
            Font = new Font("Segoe UI", 11f, FontStyle.Bold),
            ForeColor = Color.White,
            Width = ovW - 40, Height = 24,
            Margin = new Padding(0, 2, 0, 0)
        });

        /* Overlay: status */
        overlayStatus.Font = font;
        overlayStatus.ForeColor = ovDim;
        overlayStatus.Text = L.T("status.waiting");
        overlayStatus.Width = ovW - 40; overlayStatus.Height = 24;
        overlayStatus.Margin = new Padding(0, 0, 0, 8);
        ovFlow.Controls.Add(overlayStatus);

        /* Overlay: disconnect */
        ovDisconnect = new Button {
            Text = L.T("btn.disconnect"),
            Font = fontBold, ForeColor = Color.White,
            BackColor = Color.FromArgb(200, 50, 50),
            FlatStyle = FlatStyle.Flat,
            Width = ovW - 40, Height = 40,
            Cursor = Cursors.Hand,
            Margin = new Padding(0, 2, 0, 0)
        };
        ovDisconnect.FlatAppearance.BorderSize = 0;
        ovDisconnect.Click += (_,__) => { StopControl(); GoToHub(); };
        ovFlow.Controls.Add(ovDisconnect);

        OvSep();

        /* Overlay: mapping list */
        OvHead(L.T("keymap"));
        ovKeymapGrid = new TableLayoutPanel {
            Width = ovW - 40, Height = (OrderedBits.Length + 1) * 20,
            ColumnCount = 2, RowCount = OrderedBits.Length + 1,
            BackColor = Color.Transparent,
            Margin = new Padding(0, 2, 0, 0)
        };
        ovKeymapGrid.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 60));
        ovKeymapGrid.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 40));
        ovKeymapKeyLbl = new Dictionary<ushort, Label>();
        for (int i = 0; i < OrderedBits.Length; i++) {
            var bit = OrderedBits[i];
            var nameLbl = new Label {
                Text = L.T(BtnNameKey(bit)),
                Font = new Font("Segoe UI", 8.5f),
                ForeColor = ovText,
                AutoSize = false, Height = 20,
                TextAlign = ContentAlignment.MiddleLeft,
                Margin = new Padding(0)
            };
            ovKeymapGrid.Controls.Add(nameLbl, 0, i);
            ovKeymapNameLbl[bit] = nameLbl;
            var kv = new Label {
                Text = CurrentKeyName(bit),
                Font = new Font("Consolas", 8.5f),
                ForeColor = accent,
                AutoSize = false, Height = 20,
                TextAlign = ContentAlignment.MiddleRight,
                Margin = new Padding(0)
            };
            ovKeymapGrid.Controls.Add(kv, 1, i);
            ovKeymapKeyLbl[bit] = kv;
        }
        int zqsdRow = OrderedBits.Length;
        ovKeymapGrid.Controls.Add(new Label {
            Text = "ZQSD",
            Font = new Font("Segoe UI", 8.5f),
            ForeColor = ovDim,
            AutoSize = false, Height = 20,
            TextAlign = ContentAlignment.MiddleLeft,
            Margin = new Padding(0)
        }, 0, zqsdRow);
        ovZqsdLbl = new Label {
            Text = L.T("ov.zqsd"),
            Font = new Font("Consolas", 8.5f),
            ForeColor = ovDim,
            AutoSize = false, Height = 20,
            TextAlign = ContentAlignment.MiddleRight,
            Margin = new Padding(0)
        };
        ovKeymapGrid.Controls.Add(ovZqsdLbl, 1, zqsdRow);
        ovFlow.Controls.Add(ovKeymapGrid);

        OvSep();

        /* Overlay: sensitivity */
        OvHead(L.T("sensitivity"));
        ovSensitivity = new TrackBar {
            Minimum = 1, Maximum = 50000, Value = sensitivity.Value,
            Width = ovW - 40, Height = 40,
            TickFrequency = 500, SmallChange = 100, LargeChange = 1000,
            BackColor = Color.Black
        };
        ovSensitivity.ValueChanged += (_,__) => { sensitivity.Value = ovSensitivity.Value; };
        sensitivity.ValueChanged += (_,__) => { if (ovSensitivity.Value != sensitivity.Value) ovSensitivity.Value = sensitivity.Value; };
        ovFlow.Controls.Add(ovSensitivity);

        OvSep();

        /* Overlay: diagnostics */
        OvHead(L.T("ov.diag"));
        audioBox.Font = new Font("Consolas", 7.5f);
        audioBox.Multiline = true; audioBox.ReadOnly = true;
        audioBox.ScrollBars = ScrollBars.Vertical;
        audioBox.BorderStyle = BorderStyle.None;
        audioBox.BackColor = Color.FromArgb(22, 25, 31);
        audioBox.ForeColor = ovDim;
        audioBox.Text = "";
        audioBox.Width = ovW - 40;
        audioBox.Height = 110;
        audioBox.Margin = new Padding(0, 2, 0, 0);
        ovFlow.Controls.Add(audioBox);

        diag.Font = new Font("Consolas", 7.5f);
        diag.ForeColor = ovDim;
        diag.Text = "";
        diag.Width = ovW - 40; diag.Height = 170;
        diag.Margin = new Padding(0, 6, 0, 0);
        ovFlow.Controls.Add(diag);

        debugLabel.Font = new Font("Consolas", 7.5f, FontStyle.Bold);
        debugLabel.ForeColor = ovGreen;
        debugLabel.Text = "debug: waiting...";
        debugLabel.Width = ovW - 40; debugLabel.Height = 20;
        debugLabel.Margin = new Padding(0, 6, 0, 0);
        ovFlow.Controls.Add(debugLabel);

        /* ── Send loop: dedicated thread (WinForms timer dies under video message flood) ── */
        FormClosing += (_,__) => {
            timeEndPeriod(1); mouse?.Dispose(); StopControl();
            rawThreadRun = false;
            var rt = rawThread;
            if (rt != null) {
                try { PostThreadMessage((uint)rt.ManagedThreadId, 0x0012, IntPtr.Zero, IntPtr.Zero); } catch { }
                try { rt.Join(250); } catch { }
            }
        };

        mouse = new MouseHook(this);
        mouse.MouseButton += MouseButton;
    }

    void RefreshHubStatus()
    {
        hubStatus.Text = running ? L.T("status.connected") : L.T("status.waiting");
        hubStatus.Left = (Width - hubStatus.PreferredWidth) / 2;
        hubConnect.Visible = !running;
        hubDisconnect.Visible = running;
    }

    void RefreshAllText()
    {
        int W = Width, H = Height;

        infoLabel.Text = L.T("hub.info");

        settingsBtn.Text = "\u2699 " + L.T("settings.title");

        hintLbl.Text = L.T("hub.hint");
        hintLbl.Left = (W - hintLbl.PreferredWidth) / 2;

        sensLabel.Text = L.T("sensitivity");
        sensLabel.Left = (W - (int)(W * 0.25) - sensLabel.PreferredWidth) / 2;
        sensitivity.Left = sensLabel.Left + sensLabel.PreferredWidth + 10;

        hubConnect.Text = L.T("btn.connect");
        hubDisconnect.Text = L.T("btn.disconnect");
        hubStatus.Text = running ? L.T("status.connected") : L.T("status.waiting");
        hubStatus.Left = (W - hubStatus.PreferredWidth) / 2;

        settingsTitle.Text = L.T("settings.title");
        settingsTitle.Left = (W - settingsTitle.PreferredWidth) / 2;
        langLabel.Text = L.T("settings.lang");
        langLabel.Left = (W - langLabel.PreferredWidth) / 2;
        closeSettings.Text = L.T("settings.close");

        keymapTitle.Text = L.T("keymap.title");
        keymapTitle.Left = (W - keymapTitle.PreferredWidth) / 2;
        keymapHint.Text = L.T("keymap.hint");
        keymapHint.Left = (W - keymapHint.PreferredWidth) / 2;
        keymapReset.Text = L.T("keymap.reset");
        keymapBack.Text = L.T("keymap.back");
        keymapSave.Text = L.T("keymap.save");

        helpTitle.Text = L.T("help.title");
        helpTitle.Left = (W - helpTitle.PreferredWidth) / 2;
        helpBody.Text = L.T("help.content");
        helpClose.Text = L.T("settings.close");

        faqTitle.Text = L.T("faq.title");
        faqTitle.Left = (W - faqTitle.PreferredWidth) / 2;
        faqBody.Text = L.T("faq.content");
        faqClose.Text = L.T("settings.close");

        vidHint.Text = L.T("video.hint");

        overlayStatus.Text = running ? L.T("status.connected") : L.T("status.waiting");
        ovDisconnect.Text = L.T("btn.disconnect");
        ovZqsdLbl.Text = L.T("ov.zqsd");
        foreach (var kv in ovKeymapNameLbl)
            kv.Value.Text = L.T(BtnNameKey(kv.Key));

        RefreshKeymapText();
        RebuildSettingsMenu();
    }

    void RebuildSettingsMenu()
    {
        settingsMenu.SuspendLayout();
        settingsMenu.Controls.Clear();
        settingsMenu.BorderStyle = BorderStyle.None;

        Color itemHover = Color.FromArgb(225, 232, 245);
        Color itemBg = Color.FromArgb(250, 250, 252);
        int y = 4;

        void AddItem(string text, Action onClick)
        {
            var btn = new Button {
                Text = text,
                Font = new Font("Segoe UI", 10.5f),
                ForeColor = Color.FromArgb(40, 40, 40),
                Left = 0, Top = y,
                Width = settingsMenu.Width, Height = 52,
                FlatStyle = FlatStyle.Flat,
                BackColor = itemBg,
                Cursor = Cursors.Hand,
                TabStop = false
            };
            btn.FlatAppearance.BorderSize = 0;
            btn.FlatAppearance.MouseOverBackColor = itemHover;
            btn.Click += (_,__) => { settingsMenu.Visible = false; onClick(); };
            settingsMenu.Controls.Add(btn);
            y += 52;
        }

        AddItem(L.T("menu.lang"), () => ToggleSettings());
        AddItem(L.T("menu.keymap"), () => { keymapOverlay.Visible = true; keymapOverlay.BringToFront(); RefreshKeymapRows(); });
        AddItem(L.T("menu.help"), () => { helpOverlay.Visible = true; helpOverlay.BringToFront(); });
        AddItem(L.T("menu.faq"), () => { faqOverlay.Visible = true; faqOverlay.BringToFront(); });

        settingsMenu.ResumeLayout();
    }

    bool CursorInside(Control c)
    {
        if (!c.Visible) return false;
        var pt = c.PointToClient(Cursor.Position);
        return c.ClientRectangle.Contains(pt);
    }

    void RefreshKeymapText()
    {
        keymapSettingsBtn.Text = L.T("settings.keymap");
        keymapSettingsBtn.Left = (Width - keymapSettingsBtn.Width) / 2;
        BuildKeymapRows();
        RefreshKeymapRows();
    }

    void ToggleSettings()
    {
        settingsOverlay.Visible = !settingsOverlay.Visible;
        settingsOverlay.BringToFront();
        if (settingsOverlay.Visible) { keymapOverlay.Visible = false; helpOverlay.Visible = false; faqOverlay.Visible = false; }
    }

    void GoToHub()
    {
        inVideoMode = false;
        videoPanel.Visible = false;
        overlayPanel.Visible = false;
        overlayVisible = false;
        hubPanel.Visible = true;
        RefreshHubStatus();
    }

    void GoToVideo()
    {
        inVideoMode = true;
        hubPanel.Visible = false;
        settingsOverlay.Visible = false;
        helpOverlay.Visible = false;
        faqOverlay.Visible = false;
        settingsMenu.Visible = false;
        videoPanel.Visible = true;
        video.TabStop = true;
        video.Focus();
        this.ActiveControl = video;
        UpdateCursorCenter();
    }

    /* ═══════════════════════════════════════════
       CONNECTION
       ═══════════════════════════════════════════ */
    void ConnectDevice()
    {
        var conflict = FindConflict();
        if (conflict != null) {
            string n1 = L.T(BtnNameKey(conflict.Value.a));
            string n2 = L.T(BtnNameKey(conflict.Value.b));
            string kn = KeyName(conflict.Value.vk);
            MessageBox.Show(this, string.Format(L.T("keymap.error"), n1, n2, kn),
                L.T("keymap.errorTitle"), MessageBoxButtons.OK, MessageBoxIcon.Warning);
            return;
        }
        var reserved = FindReserved();
        if (reserved != null) {
            string n = L.T(BtnNameKey(reserved.Value.bit));
            string kn = KeyName(reserved.Value.vk);
            MessageBox.Show(this, string.Format(L.T("keymap.reservedError"), n, kn),
                L.T("keymap.errorTitle"), MessageBoxButtons.OK, MessageBoxIcon.Warning);
            return;
        }
        StopControl();
        device = DeviceList.Local.GetHidDevices().FirstOrDefault(d =>
            d.ProductID == 0x1337);
        if (device == null) {
            hubStatus.Text = L.T("status.notfound");
            hubStatus.Left = (Width - hubStatus.PreferredWidth) / 2;
            return;
        }
        if (!device.TryOpen(out stream) || stream == null) {
            hubStatus.Text = L.T("status.noHID");
            hubStatus.Left = (Width - hubStatus.PreferredWidth) / 2;
            return;
        }
        running = true;
        hubStatus.Text = L.T("status.connected");
        GoToVideo();
        sensCache = sensitivity.Value;
        sendRunning = true;
        sendThread = new System.Threading.Thread(SendLoop) { IsBackground = true };
        sendThread.Start();
        _ = StartVideoAsync();
        reader = new System.Threading.Thread(ReadHeartbeat) { IsBackground = true };
        reader.Start();
        aplayer = new System.Threading.Thread(AudioPlayer) { IsBackground = true };
        aplayer.Start();
    }

    void StopControl()
    {
        if (capturing) SetCapture(false);
        if (stream != null) {
            lock(sync) { buttons=0; lx=ly=rx=ry=128; mouseDx=mouseDy=0; touchDown=false; touchX=touchY=0; escDown=false; lastExitCmd=false; }
            try { SendReport(); } catch {}
        }
        StopVideoSync();
        running = false;
        sendRunning = false;
        try { sendThread?.Join(500); } catch { }
        sendThread = null;
        lock(alock) { aqHead = 0; aqTail = 0; }
        diag.Text = "";
        try { stream?.Close(); } catch {}
        stream = null; device = null;
        hubStatus.Text = L.T("status.disconnected");
    }

    /* ═══════════════════════════════════════════
       INPUT
       ═══════════════════════════════════════════ */
    int tickCount = 0;
    volatile int dbgRx, dbgRy, dbgDx, dbgDy, dbgRawEv;
    double lookVx, lookVy;
    void PollKeys()
    {
        tickCount++;
        lock(sync) {
            int x=(downKeys.Contains(0x51)?-1:0)+(downKeys.Contains(0x44)?1:0);
            int y=(downKeys.Contains(0x5A)?-1:0)+(downKeys.Contains(0x53)?1:0);
            lx=(byte)(128+x*127); ly=(byte)(128+y*127);
        }
    }

    void SendLoop()
    {
        try { timeBeginPeriod(1); } catch { }
        int tickN = 0;
        var sw = System.Diagnostics.Stopwatch.StartNew();
        long next = sw.ElapsedMilliseconds;
        while (sendRunning) {
            tickN++;
            try { PollKeys(); SendReport(); }
            catch { }
            next += 1;
            long now = sw.ElapsedMilliseconds;
            if (next <= now) next = now;
            int wait = (int)(next - now);
            if (wait > 0) try { System.Threading.Thread.Sleep(wait); } catch { }
        }
        try { timeEndPeriod(1); } catch { }
    }

    protected override void OnKeyDown(KeyEventArgs e)
    {
        base.OnKeyDown(e);
        int vk = (int)e.KeyCode;
        if (keyMap.TryGetValue(vk, out ushort bit) && downKeys.Add(vk)) {
            kbEvCount++;
            if (bit != 0) { lock(sync) { buttons |= bit; } }
        }
    }

    protected override void OnKeyUp(KeyEventArgs e)
    {
        base.OnKeyUp(e);
        int vk = (int)e.KeyCode;
        if (keyMap.TryGetValue(vk, out ushort bit) && downKeys.Remove(vk)) {
            kbEvCount++;
            if (bit != 0) { lock(sync) { buttons &= (ushort)~bit; } }
        }
        if (vk == 0x1B) { lock(sync) { escDown = false; } }
    }

    readonly HashSet<int> downKeys = new();

    Dictionary<int, ushort> keyMap = new();
    Dictionary<ushort, int> btnKeys = new();
    static readonly Dictionary<ushort, int> DefaultBtnKeys = new() {
        [SQUARE]    = 0x52,   // R
        [TRIANGLE]  = 0x45,   // E
        [CIRCLE]    = 0x10,   // Shift (L or R)
        [LTRIGGER]  = 0x70,   // F1
        [RTRIGGER]  = 0x71,   // F2
        [CROSS]     = 0x20,   // Space
        [START]     = 0x73,   // F4
        [SELECT]    = 0x72,   // F3
        [UP]        = 0x26,   // Up
        [DOWN]      = 0x28,   // Down
        [LEFT]      = 0x25,   // Left
        [RIGHT]     = 0x27,   // Right
    };
    static readonly HashSet<int> ReservedVks = new() { 0x5A, 0x51, 0x53, 0x44, 0x77, 0x7A, 0x7B, 0x1B, 0x5B, 0x5C };

    static string KeyName(int vk) => vk switch {
        0x10 => "Maj",
        0x20 => "Espace",
        0x70 => "F1",
        0x71 => "F2",
        0x72 => "F3",
        0x73 => "F4",
        0x26 => "Haut",
        0x28 => "Bas",
        0x25 => "Gauche",
        0x27 => "Droite",
        0x5B => "Win",
        0x5C => "Win",
        _ => ((System.Windows.Forms.Keys)vk).ToString()
    };

    protected override bool ProcessCmdKey(ref Message msg, Keys keyData)
    {
        if (capturePending == true) {
            int vk = (int)(keyData & Keys.KeyCode);
            if (vk == 0x1B) { EndCapture(); return true; }
            if (ReservedVks.Contains(vk)) {
                keymapWarn.Text = L.T("keymap.reserved");
                keymapWarn.ForeColor = Color.Red;
                return true;
            }
            btnKeys[captureFor] = vk;
            RebuildKeyMap();
            EndCapture();
            return true;
        }
        if (keyData == Keys.F8) { SetCapture(!capturing); return true; }
        if (keyData is Keys.F11 or Keys.F12 or Keys.Escape) {
            if (keyData == Keys.F11 && inVideoMode) {
                overlayVisible = !overlayVisible;
                overlayPanel.Visible = overlayVisible;
                if (overlayVisible) {
                    var img = video.Image;
                    ovBackdrop?.Dispose();
                    ovBackdrop = img != null ? new Bitmap(img) : null;
                    overlayPanel.Invalidate();
                }
            }
            if (keyData == Keys.F12 && inVideoMode) { StopControl(); GoToHub(); }
            if (keyData == Keys.Escape) { lock(sync) { escDown = true; escSince = DateTime.UtcNow; } }
            return true;
        }
        if ((keyData is Keys.Up or Keys.Down or Keys.Left or Keys.Right)
            && keyMap.TryGetValue((int)(keyData & Keys.KeyCode), out ushort abit) && abit != 0) {
            if (downKeys.Add((int)(keyData & Keys.KeyCode))) {
                kbEvCount++;
                lock (sync) { buttons |= abit; }
            }
            return true;
        }
        return base.ProcessCmdKey(ref msg, keyData);
    }

    void SetCapture(bool on)
    {
        if (capturing == on) return;
        capturing = on;
        if (on) {
            Cursor.Hide();
            var b = Bounds;
            var r = new RECT { Left=b.Left, Top=b.Top, Right=b.Right, Bottom=b.Bottom };
            ClipCursor(ref r);
            UpdateCursorCenter();
        } else {
            ClipCursor(IntPtr.Zero);
            Cursor.Show();
        }
    }

    void RecenterCursor()
    {
        if (cursorCenter == Point.Empty) return;
        SetCursorPos(cursorCenter.X, cursorCenter.Y);
    }

    void UpdateCursorCenter()
    {
        try { cursorCenter = video.PointToScreen(new Point(video.Width / 2, video.Height / 2)); } catch { }
    }

    void UpdateTouch(bool down, int x, int y)
    {
        lock (sync) { touchDown = down; touchX = x; touchY = y; }
    }

    void VideoTouch(object? sender, MouseEventArgs e)
    {
        touchEvCount++;
        if (capturing || !running) return;
        if (e.Button == MouseButtons.None) return;
        var img = video.Image;
        if (img == null) return;
        float iw = img.Width, ih = img.Height;
        float cw = video.ClientSize.Width, ch = video.ClientSize.Height;
        float s = Math.Min(cw / iw, ch / ih);
        float dw = iw * s, dh = ih * s;
        float ox = (cw - dw) / 2f, oy = (ch - dh) / 2f;
        float fx = (e.X - ox) / dw, fy = (e.Y - oy) / dh;
        if (fx < 0) fx = 0; if (fx >= 1) fx = 0.9999f;
        if (fy < 0) fy = 0; if (fy >= 1) fy = 0.9999f;
        int tx = Math.Clamp((int)(fx * 1920), 0, 1919);
        int ty = Math.Clamp((int)(fy * 1088), 0, 1087);
        UpdateTouch(true, tx, ty);
    }

    void SaveSensitivity()
    {
#if APPTEST
        return;
#else
        try {
            Directory.CreateDirectory(Path.GetDirectoryName(settingsPath)!);
            File.WriteAllText(settingsPath, sensitivity.Value.ToString());
        } catch { }
#endif
    }

    static string BtnNameKey(ushort bit) => bit switch {
        SQUARE => "btn.square",
        TRIANGLE => "btn.triangle",
        CIRCLE => "btn.circle",
        CROSS => "btn.cross",
        LTRIGGER => "btn.ltrigger",
        RTRIGGER => "btn.rtrigger",
        START => "btn.start",
        SELECT => "btn.select",
        UP => "btn.up",
        DOWN => "btn.down",
        LEFT => "btn.left",
        RIGHT => "btn.right",
        _ => "?"
    };

    void LoadKeyMap()
    {
#if APPTEST
        return;
#else
        try {
            if (!File.Exists(keymapPath)) return;
            var lines = File.ReadAllLines(keymapPath);
            foreach (var ln in lines) {
                var p = ln.Trim().Split('=');
                if (p.Length != 2) continue;
                var bs = p[0].Trim();
                var vs = p[1].Trim();
                if (bs.StartsWith("0x", StringComparison.OrdinalIgnoreCase)) bs = bs.Substring(2);
                if (vs.StartsWith("0x", StringComparison.OrdinalIgnoreCase)) vs = vs.Substring(2);
                if (!ushort.TryParse(bs, System.Globalization.NumberStyles.HexNumber, null, out var bit)) continue;
                if (!int.TryParse(vs, System.Globalization.NumberStyles.HexNumber, null, out int vk)) continue;
                if (btnKeys.ContainsKey(bit)) {
                    btnKeys[bit] = ReservedVks.Contains(vk) ? DefaultBtnKeys[bit] : vk;
                }
            }
        } catch { }
#endif
    }

    void SaveKeyMap()
    {
#if APPTEST
        return;
#else
        try {
            Directory.CreateDirectory(Path.GetDirectoryName(keymapPath)!);
            var sb = new System.Text.StringBuilder();
            foreach (var kv in btnKeys)
                sb.AppendLine($"0x{kv.Key:X4}=0x{kv.Value:X2}");
            File.WriteAllText(keymapPath, sb.ToString());
        } catch { }
#endif
    }

    void RebuildKeyMap()
    {
        var m = new Dictionary<int, ushort>();
        foreach (var kv in btnKeys)
            if (kv.Value != 0) m[kv.Value] = kv.Key;
        m[0x5A] = 0; m[0x51] = 0; m[0x53] = 0; m[0x44] = 0;
        keyMap = m;
        if (ovKeymapKeyLbl.Count > 0)
            foreach (var kv in ovKeymapKeyLbl)
                kv.Value.Text = CurrentKeyName(kv.Key);
    }

    bool? capturePending;
    ushort captureFor;

    void BeginCapture(ushort bit)
    {
        if (capturePending == true) EndCapture();
        capturePending = true;
        captureFor = bit;
        RefreshKeymapRows();
    }

    void EndCapture()
    {
        capturePending = null;
        RefreshKeymapRows();
    }

    (ushort a, ushort b, int vk)? FindConflict()
    {
        var byVk = new Dictionary<int, ushort>();
        foreach (var kv in btnKeys) {
            if (kv.Value == 0) continue;
            if (byVk.TryGetValue(kv.Value, out ushort other)) {
                return (other, kv.Key, kv.Value);
            }
            byVk[kv.Value] = kv.Key;
        }
        return null;
    }

    (ushort bit, int vk)? FindReserved()
    {
        foreach (var kv in btnKeys) {
            if (ReservedVks.Contains(kv.Value)) {
                return (kv.Key, kv.Value);
            }
        }
        return null;
    }

    string CurrentKeyName(ushort bit) =>
        btnKeys.TryGetValue(bit, out int vk) ? KeyName(vk) : "?";

    static readonly ushort[] OrderedBits = { SQUARE, TRIANGLE, CIRCLE, CROSS, LTRIGGER, RTRIGGER, SELECT, START, UP, DOWN, LEFT, RIGHT };

    void BuildKeymapRows()
    {
        keymapFlow.Controls.Clear();
        keymapBtns = new Dictionary<ushort, Button>();
        foreach (var bit in OrderedBits) {
            var row = new Panel {
                Width = keymapFlow.Width - 40,
                Height = 52,
                Padding = new Padding(4)
            };
            var name = new Label {
                Text = L.T(BtnNameKey(bit)),
                Font = new Font("Segoe UI", 11f, FontStyle.Bold),
                ForeColor = keymapFg,
                Left = 6, Top = 14,
                AutoSize = true
            };
            row.Controls.Add(name);
            var btn = new Button {
                Text = CurrentKeyName(bit),
                Font = new Font("Segoe UI", 11f),
                Width = 140, Height = 36,
                Top = 6, Left = row.Width - 152,
                Cursor = Cursors.Hand,
                BackColor = Color.FromArgb(230, 230, 235),
                FlatStyle = FlatStyle.Flat
            };
            btn.FlatAppearance.BorderSize = 0;
            ushort b = bit;
            btn.Click += (_,__) => {
                keymapWarn.ForeColor = Color.Red;
                if (capturePending == true) { EndCapture(); }
                BeginCapture(b);
            };
            row.Controls.Add(btn);
            keymapBtns[b] = btn;
            keymapFlow.Controls.Add(row);
        }
    }

    void RefreshKeymapRows()
    {
        if (keymapFlow.Controls.Count == 0) return;
        var conflictVk = new HashSet<int>();
        var byVk = new Dictionary<int, ushort>();
        foreach (var kv in btnKeys) {
            if (kv.Value == 0) continue;
            if (byVk.TryGetValue(kv.Value, out ushort other)) {
                conflictVk.Add(kv.Value);
                conflictVk.Add(other != kv.Key ? kv.Key : kv.Key);
            } else byVk[kv.Value] = kv.Key;
        }
        var conflictBits = new HashSet<ushort>();
        foreach (var kv in btnKeys)
            if (conflictVk.Contains(kv.Value)) conflictBits.Add(kv.Key);
        for (int i = 0; i < keymapFlow.Controls.Count; i++) {
            if (keymapFlow.Controls[i] is not Panel row) continue;
            if (row.Controls.Count < 2) continue;
            var name = (Label)row.Controls[0];
            var btn = (Button)row.Controls[1];
            ushort bit = OrderedBits[i];
            btn.Text = CurrentKeyName(bit);
            if (capturePending == true && captureFor == bit) {
                btn.Text = L.T("keymap.press");
                btn.BackColor = Color.FromArgb(255, 224, 130);
            } else if (conflictBits.Contains(bit)) {
                btn.BackColor = Color.FromArgb(255, 120, 120);
                name.ForeColor = Color.Red;
            } else {
                btn.BackColor = Color.FromArgb(230, 230, 235);
                name.ForeColor = keymapFg;
            }
        }
        if (conflictBits.Count > 0) {
            keymapWarn.Text = L.T("keymap.dupes");
            keymapWarn.ForeColor = Color.Red;
        } else if (capturePending != true) {
            keymapWarn.Text = "";
        }
    }

    [DllImport("user32.dll")] static extern bool SetCursorPos(int x, int y);
    [DllImport("winmm.dll")] static extern uint timeBeginPeriod(uint ms);
    [DllImport("winmm.dll")] static extern uint timeEndPeriod(uint ms);
    [DllImport("user32.dll")] static extern bool ClipCursor(IntPtr rect);
    [DllImport("user32.dll")] static extern bool ClipCursor(ref RECT rect);
    [StructLayout(LayoutKind.Sequential)] struct RECT { public int Left, Top, Right, Bottom; }

    protected override void OnLoad(EventArgs e)
    {
        base.OnLoad(e);
        rawHandle = GCHandle.Alloc(rawBuf, GCHandleType.Pinned);
        rawThreadRun = true;
        rawThread = new Thread(RawInputThreadMain) { IsBackground = true, Name = "RawInput" };
        rawThread.Start();
    }

    const int WM_INPUT = 0x00FF;
    protected override void WndProc(ref Message m)
    {
        base.WndProc(ref m);
    }

    void RawInputThreadMain()
    {
        var cls = "VitaSyncRawWnd2";
        rawWndProc = RawWndProc;
        var wc = new WNDCLASS {
            lpfnWndProc = Marshal.GetFunctionPointerForDelegate(rawWndProc),
            lpszClassName = cls,
            hInstance = GetModuleHandle(null)
        };
        RegisterClass(ref wc);
        var hwnd = CreateWindowEx(0, cls, "raw", 0, 0, 0, 0, 0, (IntPtr)(-3),
                                  IntPtr.Zero, GetModuleHandle(null), IntPtr.Zero);
        rawWnd = hwnd;
        if (hwnd != IntPtr.Zero) {
            var rid = new RAWINPUTDEVICE { usUsagePage = 1, usUsage = 2, dwFlags = 0x100, hwndTarget = hwnd };
            RegisterRawInputDevices(new[] { rid }, 1, (uint)Marshal.SizeOf<RAWINPUTDEVICE>());
        }
        while (rawThreadRun && rawWnd != IntPtr.Zero) {
            if (!GetMessage(out var msg, rawWnd, 0, 0)) break;
            if (msg.message == WM_INPUT) ReadRawMouse(msg.lParam);
        }
        if (rawWnd != IntPtr.Zero) { DestroyWindow(rawWnd); rawWnd = IntPtr.Zero; }
    }

    IntPtr RawWndProc(IntPtr h, uint m, IntPtr w, IntPtr l) => DefWindowProc(h, m, w, l);

    void ReadRawMouse(IntPtr hRawInput)
    {
        const uint RID_INPUT = 0x10000003;
        uint hdr = (uint)(4 + 4 + 2 * IntPtr.Size);
        uint size = 0;
        if (GetRawInputData(hRawInput, RID_INPUT, IntPtr.Zero, ref size, hdr) == unchecked((uint)-1) || size == 0) return;
        if (size > rawBuf.Length) return;
        var data = rawHandle.AddrOfPinnedObject();
        if (GetRawInputData(hRawInput, RID_INPUT, data, ref size, hdr) == unchecked((uint)-1)) return;
        int off = (int)hdr;
        if (Marshal.ReadInt16(data, off) != 0) return;
        int dx = Marshal.ReadInt32(data, off + 10);
        int dy = Marshal.ReadInt32(data, off + 14);
        if (dx != 0 || dy != 0) {
            lock (sync) { mouseDx += dx; mouseDy += dy; }
            dbgRawEv++;
        }
    }

    [DllImport("user32.dll")] static extern uint GetRawInputData(IntPtr hRawInput, uint cmd, IntPtr data, ref uint size, uint cbSizeHeader);
    [DllImport("user32.dll")] static extern bool RegisterRawInputDevices(RAWINPUTDEVICE[] devices, uint count, uint cbSize);
    [StructLayout(LayoutKind.Sequential)] struct RAWINPUTDEVICE { public ushort usUsagePage, usUsage; public uint dwFlags; public IntPtr hwndTarget; }

    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern ushort RegisterClass(ref WNDCLASS wc);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern IntPtr CreateWindowEx(uint exStyle, string cls, string name, uint style, int x, int y, int w, int h, IntPtr parent, IntPtr menu, IntPtr inst, IntPtr param);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern bool GetMessage(out NativeMessage msg, IntPtr hWnd, uint min, uint max);
    [DllImport("user32.dll")] static extern IntPtr DefWindowProc(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam);
    [DllImport("user32.dll")] static extern bool DestroyWindow(IntPtr hWnd);
    [DllImport("user32.dll")] static extern bool PostThreadMessage(uint idThread, uint msg, IntPtr wParam, IntPtr lParam);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)] static extern IntPtr GetModuleHandle(string? name);
    delegate IntPtr WndProcDelegate(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam);
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    struct WNDCLASS {
        public uint style;
        public IntPtr lpfnWndProc;
        public int cbClsExtra, cbWndExtra;
        public IntPtr hInstance, hIcon, hCursor, hbrBackground;
        [MarshalAs(UnmanagedType.LPWStr)] public string? lpszMenuName, lpszClassName;
    }
    [StructLayout(LayoutKind.Sequential)]
    struct NativeMessage { public IntPtr hwnd; public uint message; public IntPtr wParam; public IntPtr lParam; public uint time; public int ptX, ptY; }

    void StopVideoSync()
    {
        var fr = frameReader;
        if (fr != null) {
            try { fr.FrameArrived -= FrameArrived; } catch { }
            try { _ = fr.StopAsync().AsTask().Wait(300); } catch { }
            frameReader = null;
        }
        videoDeviceCleanup();
    }

    async System.Threading.Tasks.Task StartVideoAsync()
    {
        try {
            var groups = await Windows.Media.Capture.Frames.MediaFrameSourceGroup.FindAllAsync().AsTask();
            var group = groups.FirstOrDefault(g => g.DisplayName.IndexOf("PSVita", StringComparison.OrdinalIgnoreCase) >= 0)
                     ?? groups.FirstOrDefault();
            if (group == null) {
                camStat.Text = L.T("video.nosource");
                return;
            }
            mediaCapture = new Windows.Media.Capture.MediaCapture();
            await mediaCapture.InitializeAsync(new Windows.Media.Capture.MediaCaptureInitializationSettings {
                SourceGroup = group,
                SharingMode = Windows.Media.Capture.MediaCaptureSharingMode.SharedReadOnly,
                MemoryPreference = Windows.Media.Capture.MediaCaptureMemoryPreference.Cpu,
                StreamingCaptureMode = Windows.Media.Capture.StreamingCaptureMode.Video
            }).AsTask();
            frameCount = 0;
            lastFpsCheck = DateTime.UtcNow;
            var info = group.SourceInfos.FirstOrDefault(i => i.MediaStreamType == Windows.Media.Capture.MediaStreamType.VideoRecord)
                    ?? group.SourceInfos.First();
            var source = mediaCapture.FrameSources[info.Id];
            frameReader = await mediaCapture.CreateFrameReaderAsync(source).AsTask();
            frameReader.FrameArrived += FrameArrived;
            var st = await frameReader.StartAsync();
            camStat.Text = L.T("video.started", st);
        } catch (Exception ex) {
            camStat.Text = L.T("video.error", ex.Message);
            videoDeviceCleanup();
        }
    }

    void FrameArrived(Windows.Media.Capture.Frames.MediaFrameReader sender, Windows.Media.Capture.Frames.MediaFrameArrivedEventArgs args)
    {
        Interlocked.Increment(ref frameCount);
        if (framePending) return;
        using var frame = sender.TryAcquireLatestFrame();
        var sbmp = frame?.VideoMediaFrame?.SoftwareBitmap;
        if (sbmp == null) return;
        try {
            System.IDisposable? ownedConv = null;
            Windows.Graphics.Imaging.SoftwareBitmap conv;
            if (sbmp.BitmapPixelFormat == Windows.Graphics.Imaging.BitmapPixelFormat.Bgra8) {
                conv = sbmp;
            } else {
                ownedConv = Windows.Graphics.Imaging.SoftwareBitmap.Convert(
                    sbmp, Windows.Graphics.Imaging.BitmapPixelFormat.Bgra8);
                conv = (Windows.Graphics.Imaging.SoftwareBitmap)ownedConv;
            }
            try {
            int w = conv.PixelWidth, h = conv.PixelHeight;
            int stride = w * 4;
            if (frameSrcBuf == null || frameSrcBuf.Length < stride * h)
                frameSrcBuf = new byte[stride * h];
            var winrtBuf = System.Runtime.InteropServices.WindowsRuntime.WindowsRuntimeBufferExtensions.AsBuffer(
                frameSrcBuf, 0, stride * h);
            conv.CopyToBuffer(winrtBuf);

            try
            {
                const int AUD_ROWS = 8;
                const int SYMS_PER_BYTE = 4;
                const int PAYLOAD_MAX = 1728;
                const int HDR_LEN = 6;

                if (h > AUD_ROWS && w > 0) {
                    int rowStart = (h - AUD_ROWS) * w * 4;
                    int symCount = 0;
                    if (avPayload == null || avPayload.Length < HDR_LEN + PAYLOAD_MAX)
                        avPayload = new byte[HDR_LEN + PAYLOAD_MAX];
                    System.Array.Clear(avPayload, 0, HDR_LEN + PAYLOAD_MAX);
                    byte[] payload = avPayload;
                    int p = 0;

                    for (int row = 0; row < AUD_ROWS; row++) {
                        int baseIdx = rowStart + row * w * 4;
                        for (int x = 0; x < w; x++) {
                            byte g = frameSrcBuf[baseIdx + x * 4 + 1];
                            if (symCount < 16) g16[symCount] = g;
                            if (x == 0) rowG[row] = g;
                            if (g < gMin) gMin = g;
                            if (g > gMax) gMax = g;
                            byte sym;
                            if (g < 60) sym = 0;
                            else if (g < 130) sym = 1;
                            else if (g < 200) sym = 2;
                            else sym = 3;
                            int bitPos = (symCount % SYMS_PER_BYTE) * 2;
                            payload[p] |= (byte)(sym << (6 - bitPos));
                            symCount++;
                            if (symCount % SYMS_PER_BYTE == 0) {
                                p++;
                                if (p >= payload.Length) goto decode_done;
                            }
                        }
                    }
                    decode_done:
                    {
                        avW = w; avH = h;
                        tselOffDisp = -2;
                        int sigRow = -1;
                        for (int r2 = 0; r2 < AUD_ROWS; r2++) {
                            int baseIdx2 = rowStart + r2 * w * 4;
                            bool ok = true;
                            for (int x2 = 0; x2 < 8 && ok; x2++) {
                                byte gv = frameSrcBuf[baseIdx2 + x2 * 4 + 1];
                                bool hi = gv > 60, wantHi = (x2 & 1) == 0;
                                if (hi != wantHi) ok = false;
                            }
                            if (ok) { sigRow = r2; break; }
                        }
                        sigRowFound = sigRow;
                        int off = -1, offT = -1;
                        for (int i = 0; i + HDR_LEN <= p; i++) {
                            if (off < 0 && payload[i] == 'A' && payload[i + 1] == 'V') off = i;
                            if (offT < 0 && i + 7 <= p &&
                                payload[i] == 'T' && payload[i + 1] == 'S' &&
                                payload[i + 2] == 'E' && payload[i + 3] == 'L') offT = i;
                            if (off >= 0 && offT >= 0) break;
                        }
                        if (off < 0 && offT >= 8) {
                            int tlen = payload[offT + 4] | (payload[offT + 5] << 8);
                            byte tcrc = payload[offT + 6];
                            if (tlen > 0 && tlen <= PAYLOAD_MAX) {
                                int start = offT - tlen;
                                if (start < 0) start = 0;
                                int n = offT - start;
                                byte ts = 0;
                                for (int i = 0; i < n; i++)
                                    ts += payload[start + i];
                                bool crcOk = (n == tlen) && (ts == tcrc);
                                lastCrcInfo = $"tlen={tlen} n={n} crc={(crcOk ? 1 : 0)}";
                                if (n > 32) {
                                    if (avTmp == null || avTmp.Length < n) avTmp = new byte[n];
                                    Array.Copy(payload, start, avTmp, 0, n);
                                    AqWrite(avTmp, 0, n);
                                }
                            }
                        }
                        avOffset = off;
                        if (offT >= 0) tselOffDisp = offT;
                        if (DebugStringsDue(tickCount)) {
                            var sb = new System.Text.StringBuilder();
                            for (int i = 0; i < 12 && i < p; i++)
                                sb.Append(payload[i].ToString("X2"));
                            lastHexDump = sb.ToString();
                            var rg = new System.Text.StringBuilder();
                            for (int r2 = 0; r2 < AUD_ROWS; r2++)
                                rg.Append(rowG[r2]).Append(',');
                            var g1 = new System.Text.StringBuilder();
                            for (int i = 0; i < 16; i++)
                                g1.Append(g16[i]).Append(',');
                            avGeo = $"rows=[{rg}] AV={off} TSEL={offT} G16=[{g1}]";
                        }
                        if (off >= 0) {
                            avFound++;
                            int len = payload[off + 3] | (payload[off + 4] << 8);
                            byte sum = 0;
                            for (int i = 0; i < len && (off + HDR_LEN + i) < p; i++)
                                sum += payload[off + HDR_LEN + i];
                            bool crcOk = (sum == payload[off + 5]);
lastCrcInfo = $"payLen={len} crc={(crcOk ? 1 : 0)}";
                            if (crcOk && len <= PAYLOAD_MAX
                                && off + HDR_LEN + len <= p) {
                                if (avTmp == null || avTmp.Length < len) avTmp = new byte[len];
                                Array.Copy(payload, off + HDR_LEN, avTmp, 0, len);
                                AqWrite(avTmp, 0, len);
                            }
                        }
                    }
                    for (int row = 0; row < AUD_ROWS; row++) {
                        int baseIdx = rowStart + row * w * 4;
                        for (int x = 0; x < w * 4; x += 4) {
                            frameSrcBuf[baseIdx + x + 0] = 0;
                            frameSrcBuf[baseIdx + x + 1] = 0;
                            frameSrcBuf[baseIdx + x + 2] = 0;
                        }
                    }
                }
            }
            catch (Exception) { lastHexDump = "ERR"; }

            var gen = bmpGen;
            var useBmp = (vbpIdx == 0) ? videoBmp0 : videoBmp1;
            if (useBmp == null || useBmp.Width != w || useBmp.Height != h) {
                if (useBmp != null && !ReferenceEquals(useBmp, video.Image)) useBmp.Dispose();
                useBmp = new Bitmap(w, h, PixelFormat.Format32bppArgb);
                if (vbpIdx == 0) videoBmp0 = useBmp; else videoBmp1 = useBmp;
            }
            var bd = useBmp.LockBits(new Rectangle(0, 0, w, h), ImageLockMode.WriteOnly, PixelFormat.Format32bppArgb);
            try {
                if (bd.Stride == stride)
                    Marshal.Copy(frameSrcBuf, 0, bd.Scan0, stride * h);
                else
                    for (int y = 0; y < h; y++)
                        Marshal.Copy(frameSrcBuf, y * stride, bd.Scan0 + y * bd.Stride, w * 4);
            } finally { useBmp.UnlockBits(bd); }
            vbpIdx ^= 1;
            framePending = true;
            if (video.InvokeRequired) video.BeginInvoke(() => {
                framePending = false;
                if (gen != bmpGen) return;
                video.Publish(useBmp);
            });
            else { framePending = false; if (gen == bmpGen) video.Publish(useBmp); }
            } finally { ownedConv?.Dispose(); }
        } catch (ObjectDisposedException) { } catch (InvalidOperationException) { }
    }

    async System.Threading.Tasks.Task StopVideoAsync()
    {
        try {
            if (frameReader != null) {
                frameReader.FrameArrived -= FrameArrived;
                await frameReader.StopAsync();
                frameReader = null;
            }
            mediaCapture?.Dispose();
            mediaCapture = null;
        } catch { }
    }

    void videoDeviceCleanup()
    {
        try {
            if (frameReader != null) { frameReader.FrameArrived -= FrameArrived; frameReader = null; }
            mediaCapture?.Dispose();
            mediaCapture = null;
        } catch { }
        try {
            framePending = false;
            Interlocked.Increment(ref bmpGen);
            if (video.IsHandleCreated && video.InvokeRequired)
                video.BeginInvoke(new Action(() => video.Publish(null)));
            else
                video.Publish(null);
        } catch { }
        try {
            videoBmp0?.Dispose(); videoBmp0 = null;
            videoBmp1?.Dispose(); videoBmp1 = null;
        } catch { }
    }

    bool DebugStringsDue(int frameNo) => (frameNo % 25) == 0;

    void MouseButton(MouseButtons b, bool down)
    {
        if (!capturing) return;
        lock(sync) {
            if(b==MouseButtons.Left) { if(down) buttons|=LTRIGGER; else buttons&=unchecked((ushort)~LTRIGGER); }
            if(b==MouseButtons.Right){ if(down) buttons|=RTRIGGER; else buttons&=unchecked((ushort)~RTRIGGER); }
        }
    }

    void SendReport()
    {
        if(!running || stream==null) {
            return;
        }
        byte[] r=new byte[64];
        byte tflags = 0;
        int txp=0, typ=0;
        bool exitCmd;
        lock(sync) {
            int sens=sensCache;
            double dvx = 0, dvy = 0;
            if (capturing) {
                dvx = lookVx * 0.5 + mouseDx;
                dvy = lookVy * 0.5 + mouseDy;
                if (Math.Abs(dvx) < 0.5) dvx = 0;
                if (Math.Abs(dvy) < 0.5) dvy = 0;
            }
            lookVx = dvx; lookVy = dvy;
            mouseDx=mouseDy=0;
            int ax = (int)Math.Clamp(dvx, -127, 127);
            int ay = (int)Math.Clamp(dvy, -127, 127);
            dbgDx=ax; dbgDy=ay;
            int tRx=Math.Clamp(128 + ax*sens/2,0,255);
            int tRy=Math.Clamp(128 + ay*sens/2,0,255);
            rx=(byte)tRx; ry=(byte)tRy;
            dbgRx=rx; dbgRy=ry;
            r[0]=2;
            r[1]=(byte)(buttons&255); r[2]=(byte)(buttons>>8);
            r[3]=lx; r[4]=ly; r[5]=rx; r[6]=ry;
            if (touchDown) { tflags |= 1; txp=touchX; typ=touchY; }
            r[7]=tflags;
            r[8]=(byte)(txp&255); r[9]=(byte)(typ&255);
            r[10]=(byte)(((txp>>8)&0x0F)|(((typ>>8)&0x0F)<<4));
            exitCmd = escDown && (DateTime.UtcNow - escSince).TotalMilliseconds >= 600;
            r[18]=(byte)(exitCmd?1:0);
        }
        try {
            bool changed = tflags!=lastFlags || txp!=lastTx || typ!=lastTy ||
                           buttons!=lastButtons || lx!=lastLx || ly!=lastLy ||
                           rx!=lastRx || ry!=lastRy || exitCmd!=lastExitCmd;
            var nowU = DateTime.UtcNow;
            if (!changed && (nowU-lastSendUtc).TotalMilliseconds < 200) return;
            stream.Write(r);
            lastFlags=tflags; lastTx=txp; lastTy=typ;
            lastButtons=buttons; lastLx=lx; lastLy=ly; lastRx=rx; lastRy=ry;
            lastExitCmd=exitCmd;
            lastSendUtc=nowU;
        }
        catch {
            StopControl();
        }

        if ((DateTime.UtcNow - lastFpsCheck).TotalMilliseconds >= 1000) {
            int f = Interlocked.Exchange(ref frameCount, 0);
            int kbSnap = kbEvCount;
            int touchSnap = touchEvCount;
            lastFpsCheck = DateTime.UtcNow;
            BeginInvoke(() => {
                debugLabel.Text = $"hook={keyboard?.Count ?? 0}  key={kbSnap}  touch={touchSnap}\nmsg=0x{keyboard?.LastMsg:X}  vk=0x{keyboard?.LastVk:X}";
                camStat.Text = L.T("video.waiting");
            });
            kbEvCount = 0; touchEvCount = 0;
        }
        if (capturing && running) RecenterCursor();
    }

    void ReadHeartbeat()
    {
        var buf = new byte[64];
        var s = stream;
        if (s == null) return;
        try { s.ReadTimeout = 200; } catch { }
        while (running && ReferenceEquals(s, stream)) {
            try {
                int n = s.Read(buf);
                if (n < 2) continue;
                if (buf[0] == 1 && buf[1] != 0xC7) continue;
                if (n >= 28 && buf[1] == 0xC7) {
                    int ok = buf[3] | (buf[4] << 8);
                    int emu = buf[7] | (buf[8] << 8);
                    uint phys = (uint)(buf[9] | (buf[10] << 8) | (buf[11] << 16) | (buf[12] << 24));
                    int retB = (sbyte)buf[17];
                    int retA = (sbyte)buf[18];
                    int ivl = buf[24] | (buf[25] << 8) | (buf[26] << 16) | (buf[27] << 24);
                    int minX = (short)(buf[38] | (buf[39] << 8));
                    int maxX = (short)(buf[40] | (buf[41] << 8));
                    int minY = (short)(buf[42] | (buf[43] << 8));
                    int maxY = (short)(buf[44] | (buf[45] << 8));
                    string t = $"recus={ok} seq={buf[20]} err={buf[19]} act={buf[2]}\n" +
                               $"emu btns={emu:X4}   PHYS btns={phys:X8}\n" +
                               $"PHYS sticks={buf[13]}/{buf[14]} {buf[15]}/{buf[16]}\n" +
                               $"retBtn={retB} retAna={retA}\n" +
                               $"UVC: stream={buf[21]} fmt={buf[22]} frame={buf[23]} ivl={ivl}\n" +
                               $"portes: peek={buf[32]} pkR={buf[33]} read={buf[34]} rdR={buf[35]} pk2={buf[36]} rd2={buf[37]}\n" +
                               $"panneau X:{minX}..{maxX} Y:{minY}..{maxY}   svc={buf[46]}\n" +
                               $"doigt reel: vu={buf[47]&1} down={(buf[47]>>1)&1} x={buf[48]|(buf[49]<<8)} y={buf[50]|(buf[51]<<8)}\n" +
                               $"trace={Convert.ToString(buf[52],2).PadLeft(8,'0')} n={buf[53]} rx={(uint)(buf[54]|(buf[55]<<8)|(buf[56]<<16)|(buf[57]<<24))}\n" +
                               $"audio: hook={buf[60]&1} cap={(buf[60]>>1)&1} mod={(buf[60]>>2)&1}\n" +
                               $"exit: recu={buf[6]} ph={(buf[61]>>6)&3} res={(buf[61]>>4)&3} pid={buf[58]|((buf[59]&0x0F)<<8)} bloc={buf[5]}\n" +
                               $"audio: etat={buf[59]>>4} pkt={buf[62]>>4} ring={(buf[62]&0xF)*512}o\n" +
                               $"build={buf[63]}\n" +
                               $"CV: ev brut={dbgRawEv} dx={dbgDx} dy={dbgDy}  envoye RX/RY={dbgRx}/{dbgRy}  sens={sensCache} capture={capturing}";
                    BeginInvoke(() => {
                        diag.Text = t;
                        int pkt = buf[62] & 0xF;
                        int realCalls = (buf[62] >> 4) & 0xF;
                        int nullCalls = (buf[46] >> 4) & 0xF;
                        int pub = (buf[9] >> 4) & 0xF;
                        int enc = buf[9] & 0xF;
                        int port = buf[19];
                        int gprobe = (buf[60] >> 3) & 0x1F;
                        int etat = buf[59] >> 4;
                        string es = etat switch {
                            0 => "jamais appelé", 1 => "port invalide",
                            2 => "params absents→fallback", 3 => "taille non confirmée",
                            4 => "nouvelle taille apprise", 5 => "lecture OK",
                            6 => "push 0 itération!", 7 => "ring plein!",
                            8 => "rejeté: buffer NULL!", 9 => "rejeté: r<0!", _ => "?"
                        };
                        audioBox.Text = $"Audio USB (build {buf[63]}):\n" +
                            $"  queue PC: {AqCount} octets\n" +
                            $"  SONDE: magies={avFound} queue={AqCount}\n" +
                            $"  GEO {avW}x{avH} AV={avOffset} TSEL={(tselOffDisp)} | {lastCrcInfo}\n" +
                            $"  HEX: {lastHexDump} (attendu 4156)\n" +
                            $"  pub={(pub == 15 ? "15+" : $"{pub}")} enc={(enc == 15 ? "15+" : $"{enc}")} /500ms\n" +
                            $"  appels réels: {(realCalls == 15 ? "15+" : $"{realCalls}")} /500ms (dernier port={port})\n" +
                            $"  appels NULL (synchro): {(nullCalls == 15 ? "15+" : $"{nullCalls}")} /500ms\n" +
                            $"  granule port7: {(gprobe == 0 ? "NON appris !" : $"≈{(gprobe - 1) * 64} frames ✓")}\n" +
                            $"  mod_found: {(buf[60]>>2)&1}\n" +
                            $"  etat hook: {etat} ({es})\n" +
                            $"  waveOut: {(wo_opened ? "ouvert" : "fermé")}\n" +
                            $"  hook={buf[60]&1} cap={(buf[60]>>1)&1}";
                        overlayStatus.Text = L.T("status.connected") + $" (build {buf[63]})";
                        overlayStatus.ForeColor = ovOkColor;
                        avFound = 0; gMin = 999; gMax = -1;
                    });
                }
            }
            catch (TimeoutException) { }
            catch { break; }
        }
    }

    /* ---- Audio playback (waveOut, 24 kHz mono s16) ---- */
    readonly object alock = new();
    readonly byte[] aqBuf = new byte[96000];
    volatile int aqHead, aqTail;
    int AqCount { get { int h = aqHead, t = aqTail; return h >= t ? h - t : aqBuf.Length - t + h; } }
    void AqWrite(byte[] data, int offset, int count) {
        lock (alock) {
            for (int i = 0; i < count; i++) {
                aqBuf[aqHead] = data[offset + i];
                aqHead = (aqHead + 1) % aqBuf.Length;
            }
            if (AqCount > 48000) {
                int drop = AqCount - 24000;
                aqTail = (aqTail + drop) % aqBuf.Length;
            }
        }
    }
    int AqRead(IntPtr dst, int cap) {
        lock (alock) {
            int avail = AqCount;
            int take = Math.Min(cap, avail);
            if (take <= 0) return 0;
            for (int i = 0; i < take; i++) {
                Marshal.WriteByte(dst + i, aqBuf[aqTail]);
                aqTail = (aqTail + 1) % aqBuf.Length;
            }
            return take;
        }
    }
    int avFound;
    int gMin = 999, gMax = -1;
    string lastHexDump = "";
    int avOffset = -1;
    int avW, avH;
    string avGeo = "";
    int sigRowFound = -1;
    string lastCrcInfo = "";
    int tselOffDisp = -2;
    byte[]? avPayload, avTmp;
    System.Threading.Thread? aplayer;
    volatile bool wo_opened;

    [StructLayout(LayoutKind.Sequential)]
    struct WAVEFORMATEX { public ushort wFormatTag, nChannels; public uint nSamplesPerSec, nAvgBytesPerSec; public ushort nBlockAlign, wBitsPerSample, cbSize; }
    [StructLayout(LayoutKind.Sequential)]
    struct WAVEHDR { public IntPtr lpData; public uint dwBufferLength, dwBytesRecorded; public IntPtr dwUser; public uint dwFlags, dwLoops; public IntPtr lpNext, reserved; }
    [DllImport("winmm.dll")] static extern int waveOutOpen(out IntPtr h, uint devId, ref WAVEFORMATEX fmt, IntPtr cb, IntPtr inst, uint flags);
    [DllImport("winmm.dll")] static extern int waveOutPrepareHeader(IntPtr h, IntPtr hdr, int size);
    [DllImport("winmm.dll")] static extern int waveOutUnprepareHeader(IntPtr h, IntPtr hdr, int size);
    [DllImport("winmm.dll")] static extern int waveOutWrite(IntPtr h, IntPtr hdr, int size);
    [DllImport("winmm.dll")] static extern int waveOutClose(IntPtr h);
    [DllImport("winmm.dll")] static extern int waveOutReset(IntPtr h);
    static int WaveHdrSize() => Marshal.SizeOf<WAVEHDR>();

    int DequeueAudio(IntPtr dst, int cap) { return AqRead(dst, cap); }

    void WriteHdr(IntPtr wo, IntPtr hp, IntPtr dp, int len)
    {
        var hdr = new WAVEHDR { lpData = dp, dwBufferLength = (uint)len };
        Marshal.StructureToPtr(hdr, hp, false);
        waveOutPrepareHeader(wo, hp, WaveHdrSize());
        waveOutWrite(wo, hp, WaveHdrSize());
    }

    void UnprepareSafe(IntPtr wo, IntPtr hp)
    {
        for (int tries = 0; tries < 10; tries++) {
            if (waveOutUnprepareHeader(wo, hp, WaveHdrSize()) == 0) return;
            Thread.Sleep(15);
        }
    }

    void AudioPlayer()
    {
        var fmt = new WAVEFORMATEX {
            wFormatTag = 1, nChannels = 1, nSamplesPerSec = 24000,
            nAvgBytesPerSec = 48000, nBlockAlign = 2, wBitsPerSample = 16
        };
        if (waveOutOpen(out IntPtr wo, 0xFFFFFFFFu, ref fmt, IntPtr.Zero, IntPtr.Zero, 0) != 0) {
            BeginInvoke(() => audioBox.Text = L.T("audio.waveOutFail"));
            return;
        }
        wo_opened = true;
        const int HN = 4, HSZ = 4800;
        var hp = new IntPtr[HN]; var dp = new IntPtr[HN];
        for (int i = 0; i < HN; i++) {
            dp[i] = Marshal.AllocHGlobal(HSZ);
            hp[i] = Marshal.AllocHGlobal(WaveHdrSize());
        }
        uint cur = 0;
        while (running) {
            int filled = DequeueAudio(dp[cur], HSZ);
            if (filled < 600) { Thread.Sleep(10); continue; }
            WriteHdr(wo, hp[cur], dp[cur], filled);
            Thread.Sleep(Math.Max(10, (int)((long)filled * 1000 / 48000) - 5));
            UnprepareSafe(wo, hp[cur]);
            cur = (cur + 1) % HN;
        }
        for (int i = 0; i < HN; i++) {
            UnprepareSafe(wo, hp[i]);
            Marshal.FreeHGlobal(dp[i]); Marshal.FreeHGlobal(hp[i]);
        }
        waveOutClose(wo);
    }
}

internal abstract class LowLevelHook : IDisposable
{
    protected delegate IntPtr Proc(int nCode, IntPtr wParam, IntPtr lParam);
    protected Proc proc;
    IntPtr hook;
    protected LowLevelHook(int id) {
        proc=Callback;
        IntPtr hMod=LoadLibrary("user32.dll");
        hook=SetWindowsHookEx(id,proc,hMod,0);
        if(hook==IntPtr.Zero) System.Diagnostics.Debug.WriteLine($"Hook {id} FAILED err={Marshal.GetLastWin32Error()}");
    }
    protected abstract IntPtr Callback(int nCode,IntPtr wParam,IntPtr lParam);
    public void Dispose(){ if(hook!=IntPtr.Zero) UnhookWindowsHookEx(hook); hook=IntPtr.Zero; }
    [DllImport("user32.dll",SetLastError=true)] static extern IntPtr SetWindowsHookEx(int id,Proc p,IntPtr mod,uint thread);
    [DllImport("user32.dll")] static extern bool UnhookWindowsHookEx(IntPtr h);
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode)] static extern IntPtr LoadLibrary(string lpFileName);
    [DllImport("kernel32.dll")] static extern IntPtr GetModuleHandle(string? n);
}

internal sealed class KeyboardHook : LowLevelHook
{
    public event Action<Keys,bool>? KeyChanged;
    readonly HashSet<Keys> down=new();
    public volatile int Count;
    public volatile int LastMsg;
    public volatile int LastVk;
    public KeyboardHook(Form f):base(13){}
    public bool IsDown(Keys k)=>down.Contains(k);
    protected override IntPtr Callback(int n,IntPtr w,IntPtr l)
    {
        if(n>=0) {
            Count++;
            int msg=w.ToInt32(); int vk=Marshal.ReadInt32(l);
            LastMsg=msg; LastVk=vk;
            bool d=msg==0x100||msg==0x104, u=msg==0x101||msg==0x105;
            Keys k=(Keys)vk;
            if(d && down.Add(k)) KeyChanged?.Invoke(k,true);
            else if(u && down.Remove(k)) KeyChanged?.Invoke(k,false);
        }
        return CallNextHookEx(IntPtr.Zero,n,w,l);
    }
    [DllImport("user32.dll")] static extern IntPtr CallNextHookEx(IntPtr h,int n,IntPtr w,IntPtr l);
}

internal sealed class MouseHook : LowLevelHook
{
    public event Action<MouseButtons,bool>? MouseButton;
    public event Action<int,int>? MouseDelta;
    POINT? last;
    public MouseHook(Form f):base(14){}
    protected override IntPtr Callback(int n,IntPtr w,IntPtr l)
    {
        if(n>=0) {
            int msg=w.ToInt32();
            var p=Marshal.PtrToStructure<MSLLHOOKSTRUCT>(l).pt;
            if(msg==0x200) {
                if(last.HasValue) MouseDelta?.Invoke(p.X-last.Value.X,p.Y-last.Value.Y);
                last=p;
            }
            else if(msg==0x201) MouseButton?.Invoke(MouseButtons.Left,true);
            else if(msg==0x202) MouseButton?.Invoke(MouseButtons.Left,false);
            else if(msg==0x204) MouseButton?.Invoke(MouseButtons.Right,true);
            else if(msg==0x205) MouseButton?.Invoke(MouseButtons.Right,false);
        }
        return CallNextHookEx(IntPtr.Zero,n,w,l);
    }
    [StructLayout(LayoutKind.Sequential)] struct POINT{public int X,Y;}
    [StructLayout(LayoutKind.Sequential)] struct MSLLHOOKSTRUCT{public POINT pt;public uint mouseData,flags,time;public IntPtr extra;}
    [DllImport("user32.dll")] static extern IntPtr CallNextHookEx(IntPtr h,int n,IntPtr w,IntPtr l);
}

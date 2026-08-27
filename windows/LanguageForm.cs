using System;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Windows.Forms;

namespace VitaPCControl;

internal sealed class LanguageForm : Form
{
    Panel? hoveredCard;
    bool closeHover;

    public LanguageForm()
    {
Text = "VitaSync";
        WindowState = FormWindowState.Maximized;
        FormBorderStyle = FormBorderStyle.None;
        BackColor = Color.FromArgb(240, 241, 245);
        KeyPreview = true;
        DoubleBuffered = true;
        Icon = Icon.ExtractAssociatedIcon(Application.ExecutablePath);
    }

    protected override void OnLoad(EventArgs e)
    {
        base.OnLoad(e);
        Invalidate();
    }

    protected override void OnPaint(PaintEventArgs e)
    {
        base.OnPaint(e);
        var g = e.Graphics;
        g.SmoothingMode = SmoothingMode.AntiAlias;
        g.TextRenderingHint = System.Drawing.Text.TextRenderingHint.ClearTypeGridFit;

        var screen = Screen.PrimaryScreen!.WorkingArea;
        int W = screen.Width, H = screen.Height;

        /* ── Titre ── */
        using var titleFont = new Font("Segoe UI", Math.Max(18, (int)(H * 0.032f)), FontStyle.Bold);
        var titleText = "VitaSync";
        var titleSize = g.MeasureString(titleText, titleFont);
        g.DrawString(titleText, titleFont, Brushes.Black,
            (W - titleSize.Width) / 2f, H * 0.15f);

        /* ── Sous-titre langue ── */
        using var subFont = new Font("Segoe UI", Math.Max(12, (int)(H * 0.02f)));
        var subText = L.T("lang.title");
        var subSize = g.MeasureString(subText, subFont);
        g.DrawString(subText, subFont, Brushes.Gray,
            (W - subSize.Width) / 2f, H * 0.22f);

        /* ── 3 cartes ── */
        int cardW = Math.Min(260, (int)(W * 0.2));
        int cardH = (int)(cardH_ratio() * H);
        int gap = (int)(W * 0.03);
        int totalW = cardW * 3 + gap * 2;
        int startX = (W - totalW) / 2;
        int startY = (int)(H * 0.32f);

        var codes = new[] { "fr", "en", "es" };

        for (int i = 0; i < 3; i++)
        {
            string code = codes[i];
            int x = startX + i * (cardW + gap);
            var rect = new Rectangle(x, startY, cardW, cardH);
            bool hover = hoveredCard?.Tag as string == code;

            /* Ombre */
            using var shadowPath = RoundRect(rect, 12);
            g.SetClip(shadowPath);
            g.FillRectangle(new SolidBrush(Color.FromArgb(hover ? 30 : 15, 0, 0, 0)),
                rect.X + 4, rect.Y + 4, rect.Width, rect.Height);
            g.ResetClip();

            /* Fond carte */
            using var cardPath = RoundRect(rect, 12);
            g.SetClip(cardPath);
            g.FillRectangle(Brushes.White, rect);
            if (hover) {
                using var hlBrush = new SolidBrush(Color.FromArgb(20, 0, 102, 204));
                g.FillRectangle(hlBrush, rect);
            }
            g.ResetClip();

            /* Bordure */
            using var borderPen = new Pen(hover ? Color.FromArgb(0, 102, 204) : Color.FromArgb(220, 220, 225), 2f);
            g.DrawPath(borderPen, cardPath);

            /* ── Drapeau ── */
            int stripeH = (int)(cardH * 0.35f);
            var flagRect = new Rectangle(x + 1, startY + 1, cardW - 2, stripeH);
            DrawFlag(g, code, flagRect);

            /* Masquer les coins arrondis du haut de la bande */
            g.SetClip(cardPath);
            g.ResetClip();

            /* Code langue centré dans la carte */
            using var codeFont = new Font("Segoe UI", Math.Max(28, (int)(cardH * 0.14f)), FontStyle.Bold);
            var codeStr = code.ToUpperInvariant();
            var codeSize = g.MeasureString(codeStr, codeFont);
            g.DrawString(codeStr, codeFont, Brushes.Black,
                x + (cardW - codeSize.Width) / 2f, startY + stripeH + (int)(cardH * 0.1f));
        }

        /* ── Footer ── */
        using var footFont = new Font("Segoe UI", Math.Max(9, (int)(H * 0.014f)));
        var footText = "VitaSync — 2026";
        var footSize = g.MeasureString(footText, footFont);
        g.DrawString(footText, footFont, Brushes.LightGray,
            (W - footSize.Width) / 2f, H - footSize.Height - 20);

        /* ── Bouton fermer (X) en haut à droite ── */
        int btnSize = (int)(H * 0.05f);
        int btnMargin = (int)(W * 0.015f);
        var closeRect = new Rectangle(W - btnSize - btnMargin, btnMargin, btnSize, btnSize);
        if (closeHover) {
            using var hlBg = new SolidBrush(Color.FromArgb(230, 230, 235));
            g.FillEllipse(hlBg, closeRect);
        }
        using var xFont = new Font("Segoe UI", (int)(btnSize * 0.45f), FontStyle.Regular);
        var xSize = g.MeasureString("✕", xFont);
        g.DrawString("✕", xFont, closeHover ? Brushes.Black : Brushes.Gray,
            closeRect.X + (btnSize - xSize.Width) / 2f,
            closeRect.Y + (btnSize - xSize.Height) / 2f);
    }

    static void DrawFlag(Graphics g, string code, Rectangle r)
    {
        if (code == "fr") {
            /* Tricolore vertical : bleu, blanc, rouge */
            int sw = r.Width / 3;
            g.FillRectangle(new SolidBrush(Color.FromArgb(0, 35, 149)), r.X, r.Y, sw, r.Height);
            g.FillRectangle(Brushes.White, r.X + sw, r.Y, sw, r.Height);
            g.FillRectangle(new SolidBrush(Color.FromArgb(237, 41, 57)), r.X + sw * 2, r.Y, sw + 1, r.Height);
        }
        else if (code == "en") {
            /* Union Jack simplifié */
            var blue = Color.FromArgb(0, 36, 125);
            var red = Color.FromArgb(207, 0, 15);
            int cx = r.X + r.Width / 2, cy = r.Y + r.Height / 2;

            /* Fond bleu */
            g.FillRectangle(new SolidBrush(blue), r);

            /* Diagonales blanches (saltire) */
            using var whitePen = new Pen(Color.White, Math.Max(4, r.Height / 5));
            g.DrawLine(whitePen, r.Left, r.Top, r.Right, r.Bottom);
            g.DrawLine(whitePen, r.Right, r.Top, r.Left, r.Bottom);

            /* Diagonales rouges (plus fines, offset) */
            using var redDiag = new Pen(red, Math.Max(2, r.Height / 10));
            g.DrawLine(redDiag, r.Left, r.Top, r.Right, r.Bottom);
            g.DrawLine(redDiag, r.Right, r.Top, r.Left, r.Bottom);

            /* Croix blanche verticale + horizontale */
            int hw = Math.Max(4, r.Height / 4);
            g.FillRectangle(Brushes.White, cx - hw / 2, r.Top, hw, r.Height);
            g.FillRectangle(Brushes.White, r.Left, cy - hw / 2, r.Width, hw);

            /* Croix rouge verticale + horizontale (plus fine) */
            int rw = Math.Max(2, r.Height / 7);
            g.FillRectangle(new SolidBrush(red), cx - rw / 2, r.Top, rw, r.Height);
            g.FillRectangle(new SolidBrush(red), r.Left, cy - rw / 2, r.Width, rw);
        }
        else if (code == "es") {
            /* Drapeau espagnol : 3 bandes horizontales R/J/R (jaune x2) */
            int h3 = r.Height / 4;
            g.FillRectangle(new SolidBrush(Color.FromArgb(187, 0, 0)), r.X, r.Y, r.Width, h3);
            g.FillRectangle(new SolidBrush(Color.FromArgb(255, 206, 0)), r.X, r.Y + h3, r.Width, h3 * 2);
            g.FillRectangle(new SolidBrush(Color.FromArgb(187, 0, 0)), r.X, r.Y + h3 * 3, r.Width, h3 + 1);
        }
    }

    float cardH_ratio() => 0.36f;

    Rectangle GetCardRect(int i, int W, int H)
    {
        int cardW = Math.Min(260, (int)(W * 0.2));
        int cardH = (int)(cardH_ratio() * H);
        int gap = (int)(W * 0.03);
        int totalW = cardW * 3 + gap * 2;
        int startX = (W - totalW) / 2;
        int startY = (int)(H * 0.32f);
        return new Rectangle(startX + i * (cardW + gap), startY, cardW, cardH);
    }

    Rectangle GetCloseRect()
    {
        var screen = Screen.PrimaryScreen!.WorkingArea;
        int btnSize = (int)(screen.Height * 0.05f);
        int btnMargin = (int)(screen.Width * 0.015f);
        return new Rectangle(screen.Width - btnSize - btnMargin, btnMargin, btnSize, btnSize);
    }

    protected override void OnMouseClick(MouseEventArgs e)
    {
        base.OnMouseClick(e);

        if (GetCloseRect().Contains(e.X, e.Y)) {
            Application.Exit();
            return;
        }

        var screen = Screen.PrimaryScreen!.WorkingArea;
        int W = screen.Width, H = screen.Height;

        for (int i = 0; i < 3; i++)
        {
            if (GetCardRect(i, W, H).Contains(e.X, e.Y))
            {
                string lang = i switch { 0 => "fr", 1 => "en", 2 => "es", _ => "fr" };
                L.Set(lang);
                DialogResult = DialogResult.OK;
                Close();
                return;
            }
        }
    }

    protected override void OnMouseMove(MouseEventArgs e)
    {
        base.OnMouseMove(e);

        bool newCloseHover = GetCloseRect().Contains(e.X, e.Y);
        var screen = Screen.PrimaryScreen!.WorkingArea;
        int W = screen.Width, H = screen.Height;

        string? hit = null;
        for (int i = 0; i < 3; i++)
        {
            if (GetCardRect(i, W, H).Contains(e.X, e.Y)) { hit = i switch { 0 => "fr", 1 => "en", 2 => "es", _ => null }; break; }
        }

        bool changed = (hoveredCard?.Tag as string) != hit;
        if (hit == null) {
            hoveredCard = null;
        } else {
            hoveredCard = new Panel { Tag = hit };
        }
        if (changed) Invalidate();
        if (newCloseHover != closeHover) { closeHover = newCloseHover; Invalidate(); }
        Cursor = (hit != null || closeHover) ? Cursors.Hand : Cursors.Default;
    }

    protected override void OnKeyDown(KeyEventArgs e)
    {
        base.OnKeyDown(e);
        if (e.KeyCode == Keys.Escape) {
            L.Set("fr");
            DialogResult = DialogResult.OK;
            Close();
        }
    }

    static GraphicsPath RoundRect(Rectangle r, int radius)
    {
        var path = new GraphicsPath();
        int d = radius * 2;
        path.AddArc(r.X, r.Y, d, d, 180, 90);
        path.AddArc(r.Right - d, r.Y, d, d, 270, 90);
        path.AddArc(r.Right - d, r.Bottom - d, d, d, 0, 90);
        path.AddArc(r.X, r.Bottom - d, d, d, 90, 90);
        path.CloseFigure();
        return path;
    }
}

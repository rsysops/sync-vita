using System; using System.IO; using System.Windows.Forms;
namespace VitaPCControl { static class Program {
#if APPTEST
    static void CrashLog(string s) {
        try { File.AppendAllText(Path.Combine(AppContext.BaseDirectory, "crash.log"), s + Environment.NewLine); } catch { }
    }
#endif
    [STAThread] static void Main() {
        ApplicationConfiguration.Initialize();
#if APPTEST
        AppDomain.CurrentDomain.UnhandledException += (_, e) =>
            CrashLog("Unhandled: " + e.ExceptionObject);
        Application.ThreadException += (_, e) =>
            CrashLog("Thread: " + e.Exception);
#endif
        bool createdNew;
        using (var m = new System.Threading.Mutex(true, "VitaPCControl_Instance_Unique", out createdNew)) {
            if (!createdNew) {
                MessageBox.Show("VitaSync is already open (single instance only).",
                    "VitaSync", MessageBoxButtons.OK, MessageBoxIcon.Information);
                return;
            }
            if (!L.Load()) {
                Application.Run(new LanguageForm());
            }
#if APPTEST
            Application.Run(new TutoForm());
#else
            if (!TutoForm.Done) {
                Application.Run(new TutoForm());
            }
#endif
            Application.Run(new MainForm());
            m.ReleaseMutex();
        }
    } } }

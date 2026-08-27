using System; using System.Windows.Forms;
namespace VitaPCControl { static class Program {
    [STAThread] static void Main() {
        ApplicationConfiguration.Initialize();
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
            if (!TutoForm.Done) {
                Application.Run(new TutoForm());
            }
            Application.Run(new MainForm());
            m.ReleaseMutex();
        }
    } } }

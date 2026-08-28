using System;
using System.Collections.Generic;
using System.IO;

namespace VitaPCControl;

internal static class L
{
    static string _lang = "fr";
    static Dictionary<string, string> _s = new();
    static string SettingsPath => Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData),
        "VitaSync", "lang.txt");

    static readonly Dictionary<string, Dictionary<string, string>> All = new()
    {
        ["fr"] = new()
        {
            ["status.waiting"]      = "En attente de connexion USB\u2026",
            ["status.connected"]    = "Vita USB : CONNECT\u00c9",
            ["status.disconnected"] = "Vita USB : arr\u00eat\u00e9",
            ["status.notfound"]     = "Connexion USB VitaSync introuvable",
            ["status.noHID"]        = "Impossible d\u2019ouvrir le p\u00e9riph\u00e9rique HID",
            ["btn.connect"]         = "CONNECTER",
            ["btn.disconnect"]      = "D\u00c9CONNECTER",
            ["sensitivity"]         = "Sensibilit\u00e9 souris (FPS)",
            ["keymap"]              =
                "ZQSD \u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500 D\u00e9placement\n" +
                "Souris (F8) \u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500 Cam\u00e9ra FPS\n" +
                "E \u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500 \u25b3 Triangle\n" +
                "R \u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500 \u25a1 Carr\u00e9\n" +
                "Maj \u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500 \u25cb Rond\n" +
                "Espace \u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500 \u00d7 Croix\n" +
                "F1 / F2 \u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500 G\u00e2chette L / R\n" +
                "F3 / F4 \u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500 Select / Start\n" +
                "Fl\u00e8ches \u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500 D-Pad\n" +
                "F8 \u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500 Mode FPS\n" +
                "Clic image \u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500 Tactile",
            ["video.waiting"]       = "Flux vid\u00e9o : -",
            ["video.started"]       = "Flux vid\u00e9o : d\u00e9marr\u00e9 ({0})",
            ["video.nosource"]      = "Flux vid\u00e9o : aucune source vid\u00e9o trouv\u00e9e",
            ["video.error"]         = "Flux vid\u00e9o : ERREUR {0}",
            ["audio.waiting"]       = "En attente de paquets USB\u2026",
            ["audio.waveOutFail"]   = "\u00c9chec waveOutOpen",
            ["fps"]                 = "Flux vid\u00e9o : {0} img/s | Audio: {1} octets",
            ["diag.title"]          = "Vita:\n",
            ["audioBox.title"]      = "Audio USB (build {0}):\n",
            ["ov.zqsd"]             = "D\u00e9placement (r\u00e9serv\u00e9)",
            ["hub.info"] =
                "Brancher la Vita via c\u00e2ble USB\n" +
                "Installer le plugin VitaSync sur la Vita\n" +
                "Lancer un jeu sur la Vita, puis cliquer CONNECTER\n" +
                "\n" +
                "S\u00e9curit\u00e9 :\n" +
                "\u2022 Ne pas d\u00e9brancher la Vita pendant une session\n" +
                "\u2022 Utiliser un c\u00e2ble USB fiable\n" +
                "\u2022 F11 = overlay info pendant le jeu\n" +
                "\u2022 F12 = retour au menu",
            ["settings.title"]      = "Param\u00e8tres",
            ["settings.lang"]       = "Langue",
            ["settings.close"]      = "Fermer",
            ["settings.keymap"]     = "Mapper les touches\u2026",
            ["keymap.title"]        = "Mapping des touches",
            ["keymap.hint"]         = "Cliquez sur un bouton puis appuyez sur la touche \u00e0 assigner.",
            ["keymap.modify"]       = "Modifier",
            ["keymap.press"]        = "Appuyez sur une touche\u2026",
            ["keymap.reset"]        = "R\u00e9initialiser",
            ["keymap.back"]         = "Retour",
            ["keymap.save"]         = "Enregistrer",
            ["keymap.reserved"]     = "Touche r\u00e9serv\u00e9e (ZQSD / F8 / F11 / F12 / \u00c9chap / Win).",
            ["keymap.reservedError"] = "{0} est assign\u00e9 \u00e0 la touche r\u00e9serv\u00e9e {1}.\nCorrigez le mapping dans Param\u00e8tres.",
            ["keymap.dupes"]        = "Conflit : deux boutons partagent la m\u00eame touche.",
            ["keymap.error"]        = "Conflit de mapping : {0} et {1} sont tous deux assign\u00e9s \u00e0 la touche {2}.\nCorrigez le mapping dans Param\u00e8tres.",
            ["keymap.errorTitle"]   = "Mapping invalide",
            ["btn.square"]          = "Carr\u00e9",
            ["btn.triangle"]        = "Triangle",
            ["btn.circle"]          = "Rond",
            ["btn.cross"]           = "Croix",
            ["btn.ltrigger"]        = "G\u00e2chette L",
            ["btn.rtrigger"]        = "G\u00e2chette R",
            ["btn.start"]           = "Start",
            ["btn.select"]          = "Select",
            ["btn.up"]              = "Haut",
            ["btn.down"]            = "Bas",
            ["btn.left"]            = "Gauche",
            ["btn.right"]           = "Droite",
            ["video.hint"]          = "F11 = overlay  |  F12 = retour",
            ["hub.hint"]             = "F11 = overlay  |  F12 = retour  |  F8 = FPS",
            ["menu.lang"]           = "Langue",
            ["menu.keymap"]         = "Mapping des touches",
            ["menu.help"]           = "Aide",
            ["menu.faq"]            = "FAQ",
            ["help.title"]          = "Aide",
            ["help.content"] =
                "Brancher la Vita via c\u00e2ble USB\n" +
                "Le plugin VitaSync doit \u00eatre install\u00e9 sur la Vita\n" +
                "Choisir puis lancer un jeu sur la Vita, puis cliquer CONNECTER\n" +
                "\n" +
                "Raccourcis clavier :\n" +
                "\u2022 F8 = mode FPS (souris = cam\u00e9ra)\n" +
                "\u2022 F11 = overlay d\u2019informations\n" +
                "\u2022 F12 = retour au menu principal\n" +
                "\n" +
                "D\u00e9pannage :\n" +
                "\u2022 \u00c9cran noir : v\u00e9rifier le c\u00e2ble et le plugin\n" +
                "\u2022 Connexion impossible : v\u00e9rifier le mapping des touches\n" +
                "\n" +
                "Manip de s\u00e9curit\u00e9 :\n" +
                "\u2022 Toujours cliquer D\u00c9CONNECTER avant de d\u00e9brancher la Vita\n" +
                "\u2022 Ne jamais d\u00e9brancher le c\u00e2ble pendant une session active\n" +
                "\u2022 Utiliser un c\u00e2ble USB de qualit\u00e9 (court et blind\u00e9 si possible)\n" +
                "\u2022 Brancher sur un port USB direct (pas un hub)\n" +
                "\u2022 Garder la Vita charg\u00e9e : le streaming USB consomme la batterie\n" +
                "\u2022 Ne pas plier ni tordre le c\u00e2ble, \u00e9viter la surchauffe\n" +
                "\u2022 V\u00e9rifier que le plugin VitaSync est bien activ\u00e9 dans le config de la Vita",
            ["faq.title"]           = "FAQ",
            ["faq.content"] =
                "Q : L\u2019\u00e9cran vid\u00e9o reste noir, que faire ?\n" +
                "R : V\u00e9rifier le c\u00e2ble USB, le plugin VitaSync, et\n" +
                "    que le jeu est bien lanc\u00e9 avant de connecter.\n" +
                "\n" +
                "Q : Le bouton CONNECTER ne trouve rien ?\n" +
                "R : V\u00e9rifier que la Vita est branch\u00e9e et que le c\u00e2ble\n" +
                "    est bien enfonc\u00e9. R\u00e9essayer le branchement.\n" +
                "\n" +
                "Q : Comment changer la langue ?\n" +
                "R : Survolez \u2699 puis choisissez Langue.\n" +
                "\n" +
                "Q : O\u00f9 sont sauvegard\u00e9s mes r\u00e9glages ?\n" +
                "R : Dans %APPDATA%\\VitaSync\\  (keymap.txt, sensitivity.txt).\n" +
                "\n" +
                "Q : L\u2019image se fige ou lag pendant le jeu ?\n" +
                "R : Utiliser un c\u00e2ble de qualit\u00e9, un port USB direct\n" +
                "    et fermer les applications lourdes sur le PC.\n" +
                "\n" +
                "Q : Le mode FPS (F8) ne fonctionne pas ?\n" +
                "R : V\u00e9rifier la sensibilit\u00e9 dans Param\u00e8tres. Les touches\n" +
                "    r\u00e9serv\u00e9es (ZQSD, F8, F11, F12, \u00c9chap) sont bloqu\u00e9es.\n" +
                "\n" +
                "Q : F11 ne fait rien ?\n" +
                "R : L\u2019overlay d\u2019informations ne s\u2019affiche qu\u2019en jeu,\n" +
                "    pendant une session connect\u00e9e.\n" +
                "\n" +
                "Q : Certaines touches ne r\u00e9pondent pas dans le jeu ?\n" +
                "R : V\u00e9rifier le mapping dans Param\u00e8tres et cliquer sur\n" +
                "    la fen\u00eatre pour lui donner le focus.\n" +
                "\n" +
                "Q : Comment r\u00e9initialiser le mapping ?\n" +
                "R : Ouvrir Mapping des touches puis R\u00e9initialiser,\n" +
                "    ou supprimer keymap.txt dans %APPDATA%\\VitaSync\\.\n" +
                "\n" +
                "Q : La batterie de la Vita se vide vite ?\n" +
                "R : Normal : le streaming consomme. Garder la Vita\n" +
                "    branch\u00e9e ou sur secteur pendant les longues sessions.\n" +
                "\n" +
                "Q : Clavier AZERTY ou QWERTY ?\n" +
                "R : Les deux. Le d\u00e9faut utilise ZQSD mais tout peut\n" +
                "    \u00eatre r\u00e9assign\u00e9 dans Param\u00e8tres > Mapping des touches.\n" +
                "\n" +
                "Q : La connexion se coupe en plein jeu ?\n" +
                "R : Ne pas d\u00e9brancher : cliquer D\u00c9CONNECTER, v\u00e9rifier\n" +
                "    le c\u00e2ble puis reconnecter.\n" +
                "\n" +
                "Q : Le plugin VitaSync est-il requis sur la Vita ?\n" +
                "R : Oui, sans lui la vid\u00e9o et les commandes ne\n" +
                "    fonctionnent pas. L\u2019installer puis red\u00e9marrer la Vita.",
            ["lang.title"]          = "Choisissez votre langue",
            ["lang.fr"]             = "Fran\u00e7ais",
            ["lang.en"]             = "English",
            ["lang.es"]             = "Espa\u00f1ol",
            ["tuto.title"]          = "VitaSync \u2014 mini tutoriel",
            ["tuto.skip"]           = "Passer",
            ["tuto.next"]           = "Suivant",
            ["tuto.prev"]           = "Retour",
            ["tuto.finish"]         = "Terminer",
            ["tuto.dl"]             = "T\u00e9l\u00e9charger le plugin",
            ["tuto.dlDone"]         = "Plugin t\u00e9l\u00e9charg\u00e9 (copie-le sur la Vita)",
            ["tuto.dlMissing"]      = "Fichier VitaSync.skprx introuvable \u00e0 c\u00f4t\u00e9 de l\u2019application.",
            ["tuto.dlOpen"]         = "VitaSync.skprx copi\u00e9 dans :\n{0}",
            ["tuto.1title"]         = "1. Le plugin VitaSync",
            ["tuto.1body"] =
                "VitaSync = 2 parties :\n" +
                "\u2022 le plugin VitaSync.skprx \u00e0 installer sur la PS Vita\n" +
                "\u2022 cette application, sur le PC\n" +
                "\n" +
                "T\u00e9l\u00e9charge le plugin ci-dessous,\n" +
                "puis copie-le sur la carte de la Vita.",
            ["tuto.2title"]         = "2. O\u00f9 placer le plugin (sur la Vita)",
            ["tuto.2body"] =
                "Dans VitaShell (Vita branch\u00e9e en USB) :\n" +
                "\u2022 ouvre le dossier  ur0:/tai/\n" +
                "\u2022 copie  VitaSync.skprx  dedans\n" +
                "\n" +
                "Pas de dossier tai ? Appuie sur SELECT\n" +
                "dans VitaShell pour passer en USB, ou cr\u00e9e\n" +
                "le dossier ur0:/tai/.",
            ["tuto.3title"]         = "3. Activer le plugin",
            ["tuto.3body"] =
                "Dans le m\u00eame dossier, ouvre  config.txt\n" +
                "et ajoute ces 2 lignes :\n" +
                "\n" +
                "   *KERNEL\n" +
                "   ur0:tai/VitaSync.skprx\n" +
                "\n" +
                "Si ton dossier tai est sur ux0 :\n" +
                "\u00e9cris  ux0:tai/VitaSync.skprx  \u00e0 la place.",
            ["tuto.4title"]         = "4. Red\u00e9marrer la Vita",
            ["tuto.4body"] =
                "Red\u00e9marre la Vita (taiHEN lit config.txt\n" +
                "au d\u00e9marrage) puis v\u00e9rifie :\n" +
                "\u2022 VitaShell \u2192 Plugins / tai config\n" +
                "\u2022 VitaSync doit \u00eatre coch\u00e9 (ON)",
            ["tuto.5title"]         = "5. Connecter, c\u2019est parti !",
            ["tuto.5body"] =
                "Garde la Vita branch\u00e9e en USB (c\u00e2ble de\n" +
                "donn\u00e9es !), lance un jeu, puis ici :\n" +
                "\u2022 clique  CONNECTER\n" +
                "\n" +
                "F8 = souris (FPS)  \u00b7  F11 = infos  \u00b7  F12 = retour",
        },
        ["en"] = new()
        {
            ["status.waiting"]      = "Waiting for USB connection\u2026",
            ["status.connected"]    = "Vita USB: CONNECTED",
            ["status.disconnected"] = "Vita USB: stopped",
            ["status.notfound"]     = "VitaSync USB connection not found",
            ["status.noHID"]        = "Cannot open HID device",
            ["btn.connect"]         = "CONNECT",
            ["btn.disconnect"]      = "DISCONNECT",
            ["sensitivity"]         = "Mouse sensitivity (FPS)",
            ["keymap"]              =
                "ZQSD \u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500 Movement\n" +
                "Mouse (F8) \u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500 FPS camera\n" +
                "E \u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500 \u25b3 Triangle\n" +
                "R \u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500 \u25a1 Square\n" +
                "Shift \u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500 \u25cb Circle\n" +
                "Space \u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500 \u00d7 Cross\n" +
                "F1 / F2 \u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500 Trigger L / R\n" +
                "F3 / F4 \u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500 Select / Start\n" +
                "Arrows \u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500 D-Pad\n" +
                "F8 \u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500 FPS mode\n" +
                "Click image \u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500 Touch",
            ["video.waiting"]       = "Video stream: -",
            ["video.started"]       = "Video stream: started ({0})",
            ["video.nosource"]      = "Video stream: no video source found",
            ["video.error"]         = "Video stream: ERROR {0}",
            ["audio.waiting"]       = "Waiting for USB packets\u2026",
            ["audio.waveOutFail"]   = "waveOutOpen FAILED",
            ["fps"]                 = "Video stream: {0} img/s | Audio: {1} bytes",
            ["diag.title"]          = "Vita:\n",
            ["audioBox.title"]      = "Audio USB (build {0}):\n",
            ["ov.diag"]             = "Diagnostics",
            ["ov.zqsd"]             = "Movement (reserved)",
            ["hub.info"] =
                "Connect your Vita via USB cable\n" +
                "Install the VitaSync plugin on the Vita\n" +
                "Launch a game on the Vita, then click CONNECT\n" +
                "\n" +
                "Security:\n" +
                "\u2022 Do not unplug the Vita during a session\n" +
                "\u2022 Use a reliable USB cable\n" +
                "\u2022 F11 = info overlay during gameplay\n" +
                "\u2022 F12 = back to menu",
            ["settings.title"]      = "Settings",
            ["settings.lang"]       = "Language",
            ["settings.close"]      = "Close",
            ["settings.keymap"]     = "Map keys\u2026",
            ["keymap.title"]        = "Key mapping",
            ["keymap.hint"]         = "Click a button then press the key to assign.",
            ["keymap.modify"]       = "Change",
            ["keymap.press"]        = "Press a key\u2026",
            ["keymap.reset"]        = "Reset",
            ["keymap.back"]         = "Back",
            ["keymap.save"]         = "Save",
            ["keymap.reserved"]     = "Reserved key (WASD / F8 / F11 / F12 / Esc / Win).",
            ["keymap.reservedError"] = "{0} is assigned to the reserved key {1}.\nFix the mapping in Settings.",
            ["keymap.dupes"]        = "Conflict: two buttons share the same key.",
            ["keymap.error"]        = "Mapping conflict: {0} and {1} are both assigned to the key {2}.\nFix the mapping in Settings.",
            ["keymap.errorTitle"]   = "Invalid mapping",
            ["btn.square"]          = "Square",
            ["btn.triangle"]        = "Triangle",
            ["btn.circle"]          = "Circle",
            ["btn.cross"]           = "Cross",
            ["btn.ltrigger"]        = "Trigger L",
            ["btn.rtrigger"]        = "Trigger R",
            ["btn.start"]           = "Start",
            ["btn.select"]          = "Select",
            ["btn.up"]              = "Up",
            ["btn.down"]            = "Down",
            ["btn.left"]            = "Left",
            ["btn.right"]           = "Right",
            ["video.hint"]          = "F11 = overlay  |  F12 = back",
            ["hub.hint"]             = "F11 = overlay  |  F12 = back  |  F8 = FPS",
            ["menu.lang"]           = "Language",
            ["menu.keymap"]         = "Key mapping",
            ["menu.help"]           = "Help",
            ["menu.faq"]            = "FAQ",
            ["help.title"]          = "Help",
            ["help.content"] =
                "Connect your Vita via USB cable\n" +
                "The VitaSync plugin must be installed on the Vita\n" +
                "Choose then launch a game on the Vita, then click CONNECT\n" +
                "\n" +
                "Keyboard shortcuts:\n" +
                "\u2022 F8 = FPS mode (mouse = camera)\n" +
                "\u2022 F11 = information overlay\n" +
                "\u2022 F12 = back to main menu\n" +
                "\n" +
                "Troubleshooting:\n" +
                "\u2022 Black screen: check the cable and the plugin\n" +
                "\u2022 Cannot connect: check the key mapping\n" +
                "\n" +
                "Safety handling:\n" +
                "\u2022 Always click DISCONNECT before unplugging the Vita\n" +
                "\u2022 Never unplug the cable during an active session\n" +
                "\u2022 Use a good quality USB cable (short and shielded if possible)\n" +
                "\u2022 Plug into a direct USB port (not a hub)\n" +
                "\u2022 Keep the Vita charged: USB streaming drains the battery\n" +
                "\u2022 Do not bend or twist the cable, avoid overheating\n" +
                "\u2022 Make sure the VitaSync plugin is enabled in the Vita config",
            ["faq.title"]           = "FAQ",
            ["faq.content"] =
                "Q: Video screen stays black, what to do?\n" +
                "A: Check the USB cable, the VitaSync plugin, and\n" +
                "    make sure the game is running before connecting.\n" +
                "\n" +
                "Q: The CONNECT button finds nothing?\n" +
                "A: Make sure the Vita is plugged in and the cable is\n" +
                "    fully inserted. Try reconnecting.\n" +
                "\n" +
                "Q: How can I change the language?\n" +
                "A: Hover \u2699 then choose Language.\n" +
                "\n" +
                "Q: Where are my settings saved?\n" +
                "A: In %APPDATA%\\VitaSync\\  (keymap.txt, sensitivity.txt).\n" +
                "\n" +
                "Q: The image freezes or lags during gameplay?\n" +
                "A: Use a quality cable, a direct USB port, and\n" +
                "    close heavy applications on the PC.\n" +
                "\n" +
                "Q: FPS mode (F8) does not work?\n" +
                "A: Check the sensitivity in Settings. Reserved keys\n" +
                "    (WASD, F8, F11, F12, Esc) cannot be remapped.\n" +
                "\n" +
                "Q: F11 does nothing?\n" +
                "A: The info overlay only shows during gameplay,\n" +
                "    in an active session.\n" +
                "\n" +
                "Q: Some keys do not respond in the game?\n" +
                "A: Check the mapping in Settings and click on the\n" +
                "    window to give it focus.\n" +
                "\n" +
                "Q: How do I reset the mapping?\n" +
                "A: Open Key mapping then Reset, or delete keymap.txt\n" +
                "    in %APPDATA%\\VitaSync\\.\n" +
                "\n" +
                "Q: The Vita battery drains fast?\n" +
                "A: Normal: streaming consumes power. Keep the Vita\n" +
                "    plugged in during long sessions.\n" +
                "\n" +
                "Q: AZERTY or QWERTY keyboard?\n" +
                "A: Both. The default uses WASD but everything can be\n" +
                "    remapped in Settings > Key mapping.\n" +
                "\n" +
                "Q: Connection drops in the middle of a game?\n" +
                "A: Do not unplug: click DISCONNECT, check the cable,\n" +
                "    then reconnect.\n" +
                "\n" +
                "Q: Is the VitaSync plugin required on the Vita?\n" +
                "A: Yes, without it video and controls do not work.\n" +
                "    Install it then restart the Vita.",
            ["lang.title"]          = "Choose your language",
            ["lang.fr"]             = "Fran\u00e7ais",
            ["lang.en"]             = "English",
            ["lang.es"]             = "Espa\u00f1ol",
            ["tuto.title"]          = "VitaSync \u2014 quick tutorial",
            ["tuto.skip"]           = "Skip",
            ["tuto.next"]           = "Next",
            ["tuto.prev"]           = "Back",
            ["tuto.finish"]         = "Finish",
            ["tuto.dl"]             = "Download the plugin",
            ["tuto.dlDone"]         = "Plugin downloaded (copy it to the Vita)",
            ["tuto.dlMissing"]      = "VitaSync.skprx not found next to the application.",
            ["tuto.dlOpen"]         = "VitaSync.skprx saved to:\n{0}",
            ["tuto.1title"]         = "1. The VitaSync plugin",
            ["tuto.1body"] =
                "VitaSync = 2 parts:\n" +
                "\u2022 the VitaSync.skprx plugin to install on the PS Vita\n" +
                "\u2022 this application, on the PC\n" +
                "\n" +
                "Download the plugin below,\n" +
                "then copy it to the Vita's card.",
            ["tuto.2title"]         = "2. Where to put the plugin (on the Vita)",
            ["tuto.2body"] =
                "In VitaShell (Vita plugged via USB):\n" +
                "\u2022 open the folder  ur0:/tai/\n" +
                "\u2022 copy  VitaSync.skprx  inside\n" +
                "\n" +
                "No tai folder? Press SELECT in VitaShell\n" +
                "to use USB mode, or create the folder\n" +
                "ur0:/tai/.",
            ["tuto.3title"]         = "3. Enable the plugin",
            ["tuto.3body"] =
                "In the same folder, open  config.txt\n" +
                "and add these 2 lines:\n" +
                "\n" +
                "   *KERNEL\n" +
                "   ur0:tai/VitaSync.skprx\n" +
                "\n" +
                "If your tai folder is on ux0:\n" +
                "write  ux0:tai/VitaSync.skprx  instead.",
            ["tuto.4title"]         = "4. Reboot the Vita",
            ["tuto.4body"] =
                "Restart the Vita (taiHEN reads config.txt\n" +
                "at boot) then check:\n" +
                "\u2022 VitaShell \u2192 Plugins / tai config\n" +
                "\u2022 VitaSync must be checked (ON)",
            ["tuto.5title"]         = "5. Connect, let's go!",
            ["tuto.5body"] =
                "Keep the Vita plugged via USB (data\n" +
                "cable!), start a game, then here:\n" +
                "\u2022 click  CONNECT\n" +
                "\n" +
                "F8 = mouse (FPS)  \u00b7  F11 = info  \u00b7  F12 = back",
        },
        ["es"] = new()
        {
            ["status.waiting"]      = "Esperando conexi\u00f3n USB\u2026",
            ["status.connected"]    = "Vita USB: CONECTADO",
            ["status.disconnected"] = "Vita USB: detenido",
            ["status.notfound"]     = "Conexi\u00f3n USB VitaSync no encontrada",
            ["status.noHID"]        = "No se puede abrir el dispositivo HID",
            ["btn.connect"]         = "CONECTAR",
            ["btn.disconnect"]      = "DESCONECTAR",
            ["sensitivity"]         = "Sensibilidad del rat\u00f3n (FPS)",
            ["keymap"]              =
                "ZQSD \u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500 Movimiento\n" +
                "Rat\u00f3n (F8) \u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500 C\u00e1mara FPS\n" +
                "E \u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500 \u25b3 Tri\u00e1ngulo\n" +
                "R \u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500 \u25a1 Cuadrado\n" +
                "May\u00fas \u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500 \u25cb C\u00edrculo\n" +
                "Espacio \u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500 \u00d7 Cruz\n" +
                "F1 / F2 \u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500 Gatillo L / R\n" +
                "F3 / F4 \u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500 Select / Start\n" +
                "Flechas \u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500 D-Pad\n" +
                "F8 \u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500 Modo FPS\n" +
                "Clic imagen \u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500 T\u00e1ctil",
            ["video.waiting"]       = "Flujo de v\u00eddeo: -",
            ["video.started"]       = "Flujo de v\u00eddeo: iniciado ({0})",
            ["video.nosource"]      = "Flujo de v\u00eddeo: ninguna fuente de v\u00eddeo encontrada",
            ["video.error"]         = "Flujo de v\u00eddeo: ERROR {0}",
            ["audio.waiting"]       = "Esperando paquetes USB\u2026",
            ["audio.waveOutFail"]   = "waveOutOpen FALL\u00d3",
            ["fps"]                 = "Flujo de v\u00eddeo: {0} img/s | Audio: {1} bytes",
            ["diag.title"]          = "Vita:\n",
            ["audioBox.title"]      = "Audio USB (build {0}):\n",
            ["ov.diag"]             = "Diagn\u00f3stico",
            ["ov.zqsd"]             = "Movimiento (reservado)",
            ["hub.info"] =
                "Conecta tu Vita por cable USB\n" +
                "Instala el plugin VitaSync en la Vita\n" +
                "Lanza un juego en la Vita, luego pulsa CONECTAR\n" +
                "\n" +
                "Seguridad:\n" +
                "\u2022 No desconectar la Vita durante una sesi\u00f3n\n" +
                "\u2022 Usa un cable USB fiable\n" +
                "\u2022 F11 = overlay de info durante el juego\n" +
                "\u2022 F12 = volver al men\u00fa",
            ["settings.title"]      = "Ajustes",
            ["settings.lang"]       = "Idioma",
            ["settings.close"]      = "Cerrar",
            ["settings.keymap"]     = "Asignar teclas\u2026",
            ["keymap.title"]        = "Asignaci\u00f3n de teclas",
            ["keymap.hint"]         = "Haz clic en un bot\u00f3n y luego pulsa la tecla a asignar.",
            ["keymap.modify"]       = "Cambiar",
            ["keymap.press"]        = "Pulsa una tecla\u2026",
            ["keymap.reset"]        = "Restablecer",
            ["keymap.back"]         = "Volver",
            ["keymap.save"]         = "Guardar",
            ["keymap.reserved"]     = "Tecla reservada (WASD / F8 / F11 / F12 / Esc / Win).",
            ["keymap.reservedError"] = "{0} est\u00e1 asignado a la tecla reservada {1}.\nCorrige la asignaci\u00f3n en Ajustes.",
            ["keymap.dupes"]        = "Conflicto: dos botones comparten la misma tecla.",
            ["keymap.error"]        = "Conflicto de asignaci\u00f3n: {0} y {1} est\u00e1n asignados a la tecla {2}.\nCorrige la asignaci\u00f3n en Ajustes.",
            ["keymap.errorTitle"]   = "Asignaci\u00f3n no v\u00e1lida",
            ["btn.square"]          = "Cuadrado",
            ["btn.triangle"]        = "Tri\u00e1ngulo",
            ["btn.circle"]          = "C\u00edrculo",
            ["btn.cross"]           = "Cruz",
            ["btn.ltrigger"]        = "Gatillo L",
            ["btn.rtrigger"]        = "Gatillo R",
            ["btn.start"]           = "Start",
            ["btn.select"]          = "Select",
            ["btn.up"]              = "Arriba",
            ["btn.down"]            = "Abajo",
            ["btn.left"]            = "Izquierda",
            ["btn.right"]           = "Derecha",
            ["video.hint"]          = "F11 = overlay  |  F12 = volver",
            ["hub.hint"]             = "F11 = overlay  |  F12 = volver  |  F8 = FPS",
            ["menu.lang"]           = "Idioma",
            ["menu.keymap"]         = "Asignaci\u00f3n de teclas",
            ["menu.help"]           = "Ayuda",
            ["menu.faq"]            = "FAQ",
            ["help.title"]          = "Ayuda",
            ["help.content"] =
                "Conecta tu Vita por cable USB\n" +
                "El plugin VitaSync debe estar instalado en la Vita\n" +
                "Elige y lanza un juego en la Vita, luego pulsa CONECTAR\n" +
                "\n" +
                "Atajos de teclado:\n" +
                "\u2022 F8 = modo FPS (rat\u00f3n = c\u00e1mara)\n" +
                "\u2022 F11 = overlay de informaci\u00f3n\n" +
                "\u2022 F12 = volver al men\u00fa principal\n" +
                "\n" +
                "Soluci\u00f3n de problemas:\n" +
                "\u2022 Pantalla negra: revisa el cable y el plugin\n" +
                "\u2022 No conecta: revisa la asignaci\u00f3n de teclas\n" +
                "\n" +
                "Manipulaciones de seguridad:\n" +
                "\u2022 Pulsa siempre DESCONECTAR antes de desconectar la Vita\n" +
                "\u2022 No desconectes nunca el cable durante una sesi\u00f3n activa\n" +
                "\u2022 Usa un cable USB de calidad (corto y blindado si es posible)\n" +
                "\u2022 Con\u00e9ctalo a un puerto USB directo (no un hub)\n" +
                "\u2022 Mant\u00e9n la Vita cargada: el streaming USB gasta bater\u00eda\n" +
                "\u2022 No dobles ni retuerzas el cable, evita el sobrecalentamiento\n" +
                "\u2022 Comprueba que el plugin VitaSync est\u00e9 activado en la config de la Vita",
            ["faq.title"]           = "FAQ",
            ["faq.content"] =
                "P: La pantalla de v\u00eddeo sigue en negro, \u00bfqu\u00e9 hago?\n" +
                "R: Revisa el cable USB, el plugin VitaSync y que el\n" +
                "    juego est\u00e9 iniciado antes de conectar.\n" +
                "\n" +
                "P: El bot\u00f3n CONECTAR no encuentra nada?\n" +
                "R: Aseg\u00farate de que la Vita est\u00e1 enchufada y el cable\n" +
                "    bien insertado. Prueba a reconectar.\n" +
                "\n" +
                "P: \u00bfC\u00f3mo cambio el idioma?\n" +
                "R: Pasa el rat\u00f3n sobre \u2699 y elige Idioma.\n" +
                "\n" +
                "P: \u00bfD\u00f3nde se guardan mis ajustes?\n" +
                "R: En %APPDATA%\\VitaSync\\  (keymap.txt, sensitivity.txt).\n" +
                "\n" +
                "P: \u00bfLa imagen se congela o va lenta en el juego?\n" +
                "R: Usa un cable de calidad, un puerto USB directo y\n" +
                "    cierra las aplicaciones pesadas en el PC.\n" +
                "\n" +
                "P: \u00bfEl modo FPS (F8) no funciona?\n" +
                "R: Revisa la sensibilidad en Ajustes. Las teclas\n" +
                "    reservadas (WASD, F8, F11, F12, Esc) est\u00e1n bloqueadas.\n" +
                "\n" +
                "P: \u00bfF11 no hace nada?\n" +
                "R: El overlay de info solo aparece durante el juego,\n" +
                "    en una sesi\u00f3n activa.\n" +
                "\n" +
                "P: \u00bfAlgunas teclas no responden en el juego?\n" +
                "R: Revisa la asignaci\u00f3n en Ajustes y haz clic en la\n" +
                "    ventana para darle el foco.\n" +
                "\n" +
                "P: \u00bfC\u00f3mo restablezco la asignaci\u00f3n?\n" +
                "R: Abre Asignaci\u00f3n de teclas y pulsa Restablecer, o\n" +
                "    borra keymap.txt en %APPDATA%\\VitaSync\\.\n" +
                "\n" +
                "P: \u00bfLa bater\u00eda de la Vita se vac\u00eda r\u00e1pido?\n" +
                "R: Normal: el streaming consume. Mant\u00e9n la Vita\n" +
                "    enchufada durante sesiones largas.\n" +
                "\n" +
                "P: \u00bfTeclado AZERTY o QWERTY?\n" +
                "R: Ambos. El predeterminado usa WASD pero todo puede\n" +
                "    reasignarse en Ajustes > Asignaci\u00f3n de teclas.\n" +
                "\n" +
                "P: \u00bfLa conexi\u00f3n se corta a mitad de partida?\n" +
                "R: No desconectes: pulsa DESCONECTAR, revisa el cable\n" +
                "    y vuelve a conectar.\n" +
                "\n" +
                "P: \u00bfEs obligatorio el plugin VitaSync en la Vita?\n" +
                "R: S\u00ed, sin \u00e9l el v\u00eddeo y los controles no funcionan.\n" +
                "    Inst\u00e1lalo y reinicia la Vita.",
            ["lang.title"]          = "Elige tu idioma",
            ["lang.fr"]             = "Fran\u00e7ais",
            ["lang.en"]             = "English",
            ["lang.es"]             = "Espa\u00f1ol",
            ["tuto.title"]          = "VitaSync \u2014 mini tutorial",
            ["tuto.skip"]           = "Saltar",
            ["tuto.next"]           = "Siguiente",
            ["tuto.prev"]           = "Atr\u00e1s",
            ["tuto.finish"]         = "Terminar",
            ["tuto.dl"]             = "Descargar el plugin",
            ["tuto.dlDone"]         = "Plugin descargado (c\u00f3pialo a la Vita)",
            ["tuto.dlMissing"]      = "VitaSync.skprx no encontrado junto a la aplicaci\u00f3n.",
            ["tuto.dlOpen"]         = "VitaSync.skprx guardado en:\n{0}",
            ["tuto.1title"]         = "1. El plugin VitaSync",
            ["tuto.1body"] =
                "VitaSync = 2 partes:\n" +
                "\u2022 el plugin VitaSync.skprx para instalar en la PS Vita\n" +
                "\u2022 esta aplicaci\u00f3n, en el PC\n" +
                "\n" +
                "Descarga el plugin a continuaci\u00f3n\n" +
                "y c\u00f3pialo a la tarjeta de la Vita.",
            ["tuto.2title"]         = "2. D\u00f3nde poner el plugin (en la Vita)",
            ["tuto.2body"] =
                "En VitaShell (Vita conectada por USB):\n" +
                "\u2022 abre la carpeta  ur0:/tai/\n" +
                "\u2022 copia  VitaSync.skprx  dentro\n" +
                "\n" +
                "\u00bfNo hay carpeta tai? Pulsa SELECT en VitaShell\n" +
                "para usar USB, o crea la carpeta ur0:/tai/.",
            ["tuto.3title"]         = "3. Activar el plugin",
            ["tuto.3body"] =
                "En la misma carpeta, abre  config.txt\n" +
                "y a\u00f1ade estas 2 l\u00edneas:\n" +
                "\n" +
                "   *KERNEL\n" +
                "   ur0:tai/VitaSync.skprx\n" +
                "\n" +
                "Si tu carpeta tai est\u00e1 en ux0:\n" +
                "escribe  ux0:tai/VitaSync.skprx.",
            ["tuto.4title"]         = "4. Reiniciar la Vita",
            ["tuto.4body"] =
                "Reinicia la Vita (taiHEN lee config.txt al\n" +
                "arrancar) y comprueba:\n" +
                "\u2022 VitaShell \u2192 Plugins / tai config\n" +
                "\u2022 VitaSync debe estar marcado (ON)",
            ["tuto.5title"]         = "5. Conectar, \u00a1vamos!",
            ["tuto.5body"] =
                "Mant\u00e9n la Vita por USB (\u00a1cable de datos!),\n" +
                "inicia un juego y, aqu\u00ed:\n" +
                "\u2022 haz clic en  CONECTAR\n" +
                "\n" +
                "F8 = rat\u00f3n (FPS)  \u00b7  F11 = info  \u00b7  F12 = volver",
        },
    };

    public static string Lang => _lang;

    public static string T(string key) =>
        _s.TryGetValue(key, out var v) ? v : key;

    public static string T(string key, object arg0) =>
        string.Format(T(key), arg0);

    public static string T(string key, object arg0, object arg1) =>
        string.Format(T(key), arg0, arg1);

    public static bool Load()
    {
#if APPTEST
        _lang = "fr";
        _s = All[_lang];
        return false;
#else
        try {
            var t = File.ReadAllText(SettingsPath).Trim().ToLowerInvariant();
            if (All.ContainsKey(t)) { _lang = t; _s = All[t]; return true; }
        } catch { }
        return false;
#endif
    }

    public static void Save()
    {
#if APPTEST
        return;
#else
        try {
            Directory.CreateDirectory(Path.GetDirectoryName(SettingsPath)!);
            File.WriteAllText(SettingsPath, _lang);
        } catch { }
#endif
    }

    public static void Set(string lang)
    {
        _lang = lang;
        _s = All.TryGetValue(lang, out var d) ? d : All["fr"];
        Save();
    }
}

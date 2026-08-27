# VitaSync — Streaming & contrôle PS Vita via USB

Streamer la vidéo de la PS Vita sur PC **et** piloter la console (stick, boutons,
tactile, gâchettes) par USB, le tout dans une seule application Windows.

Prototype basé sur le fork modifié de **`vita-udcd-uvc` 1.7** (`xerpi/vita-udcd-uvc`, licence MIT).

## Deux composants

- `VitaSync/` — plugin kernel Vita (`VitaSync.skprx`) : UVC + interface
  HID vendor USB. Nom UVC du device : `VITASYNC`, PID USB `0x1337`.
- `windows/` — application C#/.NET 8 WinForms (`VitaSync.exe`) : vidéo Media
  Foundation, HID (HidSharp), mapping clavier/souris, tactile, mode FPS F8,
  tutoriel d'installation intégré (5 pages, FR/EN/ES).

## Installer le plugin sur la Vita

1. Copier `VitaSync.skprx` dans `ur0:tai/` (VitaShell).
2. Ajouter au `ur0:tai/config.txt` :

   ```
   *KERNEL
   ur0:tai/VitaSync.skprx
   ```

3. Redémarrer complètement la Vita.

> Le bouton de téléchargement du tutoriel récupère automatiquement le plugin
> embarqué et le copie dans `Téléchargements`.

## Compiler l'application Windows

```powershell
cd windows
dotnet build -c Release
```

Exécutable : `windows\bin\Release\net8.0-windows10.0.22621.0\VitaSync.exe`
(fichier `VitaPCControl.csproj` = nom de projet historique, l'exe est `VitaSync.exe`).

Réglages utilisateur : `%APPDATA%\VitaSync\` (`sensitivity.txt`, `lang.txt`,
`keymap.txt`, `tutoDone.txt`). Le tutoriel n'apparaît qu'une fois (via `tutoDone.txt`).

## Divers

- Dossier `build` / `bin` volontairement ignorés par Git (artefacts de compilation).

## ⚠️ Avertissement

Prototype non testé comme release officielle. À tester sur une vraie Vita avant
utilisation. Le plugin est chargé en kernel — risque de blocage (secours :
redémarrer la Vita en maintenant **L** pour sauter les plugins).
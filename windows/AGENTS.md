# Règles du projet (VitaPCControl)

- Langue de l'utilisateur : français ; messages en français.
- Après CHAQUE modification de code (MainForm.cs, Language.cs, etc.), recompiler automatiquement :
  `dotnet build "C:\Users\<user>\Desktop\Développement app\VitaPCControl_USB_Prototype_01\windows" -c Release --nologo -v q`
  et vérifier qu'il y a 0 erreur.
- Ne pas demander confirmation pour recompiler : le faire systématiquement.
- Ne PAS committer/pousser sans demande explicite.
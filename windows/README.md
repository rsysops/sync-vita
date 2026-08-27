# VitaSync - Windows app
This Windows application sends the 16-byte HID output report used by the modified Vita USB plugin (`VitaSync.skprx`).
Build with .NET 8 SDK on Windows 11:
dotnet restore
dotnet build -c Release

The application searches for HID Usage Page 0xFF00 / Usage 0x01.

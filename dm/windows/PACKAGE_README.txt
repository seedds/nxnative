BUILD A MAC APP OF THIS GAME ON WINDOWS

This turns the game's .nsp into a Mac app on this PC, using the real build script.
It changes no Windows settings and writes only inside this folder (Python's own
download cache aside). Your game and keys are not in this package and are left out
of results.zip. app.zip holds the game, so keep it on your own machines.

BEFORE THE FIRST RUN
1. Install Python 3.14 (not 3.15) from python.org: the latest 3.14 Windows
   installer, 64-bit. It can sit beside another Python version.
2. Install the .NET 10 SDK from dot.net/download (the SDK, not just the runtime).
3. The Metal tools from the earlier test are already installed.
4. Stay online for the first run: it downloads a few Python modules and what the
   game extractor needs. It also needs about 20 GB free.

RUN IT
1. Unzip this package to a short folder on C:, such as C:\dmb
2. Copy the game's .nsp into  C:\dmb\game\
3. Copy your prod.keys into  C:\dmb\keys\
4. Double-click RUN_BUILD.bat. It shows each step as it goes.
   If Windows shows "Windows protected your PC", click "More info", then "Run anyway".

AFTERWARDS
Copy these to the Mac's shared folder, windows-test, and tell Claude:
  results.zip   small; made even if the build stops early, and it says why
  app.zip       about 5.4 GB; only made if the build finished

If it stops early, that is still useful. Send results.zip.

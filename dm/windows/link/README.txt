LINK THIS LAPTOP TO THE MAC, SO CLAUDE CAN RUN THE BUILDS DIRECTLY

What it does: turns on Windows' own OpenSSH Server. Only the project's Mac, holding its
key, can connect; passwords are off; only computers on your local network can reach it,
and only on a network Windows treats as Private. The Mac signs in to a limited account
called nxbuild, which can use C:\dmb but cannot change Windows settings or open your
personal files. The service stays on after a restart until you undo it.

SET UP
1. Copy this folder to the laptop, for example to C:\laptop-ssh
   (run it from the C: drive, not from the shared folder).
2. Read setup.ps1 if you would like to: it is plain text.
3. Right-click SETUP.bat and choose "Run as administrator".
4. If it asks about making your network Private, answer y only for your own home network.
5. Tell Claude the name and address it prints at the end.

Also: set the laptop not to sleep while plugged in, or a build can stop halfway.

UNDO
Right-click UNDO.bat and choose "Run as administrator". It removes the account, the
firewall rule, the key and (if the setup installed it) the OpenSSH Server.

# Prewarm inbox

The Apps Script web app accepts `POST /exec?title=<title ID>&version=<game version>` with `Content-Type: text/tab-separated-values`. It validates `pso_user.tsv` up to 32 MiB, then files it in the owner's private Drive folder by title ID, version, and content hash. An identical upload returns the same receipt.

Create a Google Apps Script project, copy in `Code.gs` and `appsscript.json`, and run `setupInbox` once as the account that will own the folder. Deploy it as a web app that runs as the owner and accepts anonymous requests. The script uses the Drive API's `drive.file` scope for files it creates.

Put the deployed `https://script.google.com/macros/s/…/exec` URL in `endpoint.txt` for normal package builds. Set `DM_PREWARM_UPLOAD_URL` to override it. The app acknowledges an upload only when Google's receipt matches the submitted file's hash.

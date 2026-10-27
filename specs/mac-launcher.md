# Mac launcher

The game's Mac app shows one native grouped settings form before starting the host.
The table `dm/games/settings.tsv` owns each row's platform, section and default; its optional `meta` column carries `level` and `needs`.

- Basic contains resolution, display mode, frame rate, console mode, game language and game rumble, when supported.
- A native "Advanced settings" disclosure follows Basic. It remembers its open state per app.
- Advanced contains every other setting, grouped under section headings below the disclosure.
- Expansion keeps the Basic rows and disclosure in place. The window fits its content, capped to the screen's visible frame, then scrolls.
- The footer stays on one line: "Show this window at launch", Quit, then default Play.
- iOS keeps its existing single-list layout and ignores row levels.
- Both platforms hide sub-settings while their `needs` value does not match, and omit sections with no visible rows.
- The Mac's resolution pixels use `launcher.json`'s `base`, from the profile's `render_base`.
- Launch environment and environment files keep precedence. DM_LAUNCHER=0 and --no-launcher skip the window with saved choices; Option overrides a saved hidden window.
- DM_SAVED_CHOICES=0 ignores the shared saved-choice store without changing it.

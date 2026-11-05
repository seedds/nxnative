# Charter

> **If you read nothing else**
> 1. **Start from the latest `main`, and look for existing work first**, every time: `git fetch` before a new branch or an issue (it may be fixed already), then pull requests and issues by game and area label and by title, active branches ([how](agents/06-dev-loop.md#0-before-you-start-existing-work-then-a-branch)).
> 2. **Make your own work findable:** a draft pull request early, with the game's name and title ID in its title.
> 3. **Don't make the base Macs worse:** a change that costs an 8 GB M1 (Switch games) or a 16 GB M2 (Switch 2 games) is a regression, even when it helps the Mac it was made on.
> 4. **No regressions:** a gain on one Mac paid for by another is not a gain.
> 5. **The bar rises with reach:** a change to one game shows that game; a change to a shared path shows more ([tiers](#pull-requests)).

Many people work on nxnative, mostly through agents, usually on one game or subsystem at a time. This page says what the project is for, what a change must not break, and what to check first, so that work on one part does not cost another. [AGENTS.md](../AGENTS.md) links here; [06-dev-loop.md](agents/06-dev-loop.md) has the commands.

**What this page does not ask.** You do not need a base Mac, and you do not need to work on low-spec performance. Bringing up games on whatever Mac you have, and features that use a fast Mac's headroom, are welcome. What is asked is narrower: shared code does not get worse on the base Macs, and a measurement names the Mac it was made on.

Most of the work is agentic, which makes it cheap to start and easy to multiply. That is a strength when the sessions add up: each one knows what exists and what is under way, picks the piece that moves a goal, builds on what others have done, and leaves a record the next one can find.

## Aim

Play your own Switch and Switch 2 games on the iPhone, iPad and Mac by running the game's own code natively ([README](../README.md)), **as they play on the console**: same picture and sound, same feel in the hands, same pacing, same relation to real time.

The project is the host, the rewriter, the shader chain and the tools that serve every game. Each game brought up finds and tests a part of them, and the general part of its fixes is what other games inherit: a fix is done when it holds for every game it reaches.

## Correctness

A game is correct when it plays as on the console, not only when it looks right:

- **Picture and sound** as the console gives them.
- **Smoothness:** even frame pacing at the game's own rate beats a higher average; a steady 30 beats a stuttering 30 to 60. The game's frame-rate controller and dynamic resolution behave as on the console.
- **Time:** games that follow the real clock (Animal Crossing's days and seasons, timed events) see the real date and time, through sleep and suspension too.
- **Input:** the console's own controllers (Joy-Con, Pro Controller, the Switch 2's) work with their buttons, motion and rumble, as do other controllers.
- **Gameplay** at the console's speed: no fast-forward, no slow motion.

## Targets for the shared code

The bars the host, the translator, the tools and the defaults every game gets are measured against. They are not what each contributor must work on: they name the Macs most people play on, so that a shared change that helps a fast Mac and costs a slow one is seen as a cost.

- **Every Switch game playable on an M1 Mac with 8 GB.** Benchmark: [Tears of the Kingdom](../dm/games/0100F2C0115B6000/README.md), recent and among the console's heaviest.
- **Every Switch 2 game playable on an M2 Mac with 16 GB and a 10-core GPU**, settings lowered where needed (render scale, console mode, upscaler). Benchmark, once it plays: Ocarina of Time (Switch 2, `0400486024DEC000`), the most demanding Switch 2 game brought up so far.

Where each game stands, and what holds it back on these Macs, is in its note ([games table](../dm/games/README.md#games)).

**Playable** on a given Mac: correct as above from the start through normal play, no crash or hang, at the game's intended frame rate most of the time and evenly paced, no paging that stalls play, with settings the launcher offers. A game in the [games table](../dm/games/README.md#games) is playable on the Mac it was tested on; whether it is playable on the base Mac, and what stands in the way, goes in its note when known. A change that moves a game away from either is a regression.

**Why it pays everywhere:** less CPU and GPU time a frame is longer battery life on every MacBook, iPhone and iPad; on Pro and Max Macs it is headroom for higher frame rates (120 fps where a game allows) and better upscaling; and what fits an 8 GB Mac is close to what fits a phone.

## Devices

A change is judged by what it does on the lowest device of each platform it reaches, as well as on the device it was developed on: a gain there does not make up for a cost on the lowest.

| Platform | Lowest supported |
|---|---|
| iPhone | iPhone 15 Pro (A17 Pro, 8 GB), the README's floor |
| iPad | M1 iPad |
| Mac, Switch games | M1, 8 GB (a Switch has 4 GB) |
| Mac, Switch 2 games | M2, 16 GB, 10-core GPU (a Switch 2 has 12 GB). An M1 with 16 GB or more may also get there, with fewer GPU cores |

Most Macs people play on are base chips with 8 or 16 GB: about two thirds of Apple silicon Macs on Steam, and a third or more likely still 8 GB ([appendix](#appendix-the-macs-people-play-on)). CPU and GPU share that memory, so memory is the limit these Macs meet first.

A change about speed, memory or power measured only above the floor says so, and is not called a gain for everyone.

## Priorities

When these conflict, the earlier wins. This is the order a conflict is decided in, not an order of whose work matters:

1. **Correctness**, in every sense above, in a game that worked.
2. **No regressions:** what worked keeps working, as smoothly and in as little memory, on every supported device.
3. **Performance on the lowest devices:** pacing, frame rate, memory, load time, power.

Performance is a goal in itself, not only for the lowest devices. Settings let each device find its own balance: a base Mac lowers the render scale, plays in handheld mode or turns off an extra, while a Pro or Max Mac uses the headroom for higher resolution, better upscaling or 120 fps. So a feature for faster Macs is welcome **when it leaves the slower ones as they were**: behind a setting or a switch, or scaled to the device. What is not acceptable is a gain on one class of Mac paid for by another, for example:

- a cache or buffer sized for a 24 GB Mac that pushes every 16 GB Mac into paging;
- a path tuned on a Pro or Max chip that drops every base chip from 60 to 30 fps;
- a default that helps one chip's GPU clock and costs the others' battery.

Not goals unless the maintainers decide otherwise: features a console lacks (save states, rewind), 32-bit ARM games, online play.

## Contributions wanted

Game support is welcome; everyone wants their favourite game working. The project also asks for work that serves every game:

- **Optimization:** CPU per frame on the submit path, memory, pacing, load times, power, measured on the lowest devices.
- **Shared correctness:** a host service, translator instruction or replay rule fixed for every game that uses it.
- **Measurement:** smoke routes and baselines for your games, and regression reports on games other than your own.
- **Testing on a low-spec device:** no code needed (below).
- **Tooling and docs** that make the next game, or the next agent, cheaper.

When you bring up a game, the general part of each fix is what lasts; say which other games it reaches.

## Testing on the lowest devices

Few contributors own the lowest device of each platform. A pull request whose result depends on one asks with a **Test request on a low-spec device** issue (the form in `.github/ISSUE_TEMPLATE`): what to run, where, and what to report. Anyone who owns such a device can answer in a few minutes.

A request does not hold a pull request forever: if no report has come after a week, the pull request goes ahead saying it was measured only above the floor, and the maintainers decide. The same holds for a review the tier asks for.

## What counts as a regression

Whatever `scripts/regress.py check` reports as `NEW-FAIL` or `REGRESSION` against the merge base (shaders, crashes, hangs, the route's frame, new `[RED]` lines, screenshots, frame time, memory, prepare time), and beyond what it sees:

- **Smoothness:** frame-time p95 or late frames worse in a played scene, even when the median holds.
- **Memory:** resident memory higher in a played scene on the lowest Mac or iPhone.
- **CPU per frame:** time added to the submit thread, the limit of several Switch 2 games on base Macs, even when a faster Mac hides it.
- **Time, input, gameplay:** a game's clock, a controller's buttons, motion or rumble, or its speed changed from the console's.
- **A default changed for every game** without the evidence below.

A regression accepted on purpose is stated in the pull request with its cost and reason; the maintainers decide.

## Before starting work

1. **Look for work already going on, every time:** pull requests and issues by game label (`game: <title ID>`) and area label (`nvn`, `host`, `shader chain`…), then by title ID and name in their titles; active branches and recent commits to the same files; the game's note and the [symptom index](agents/symptoms.md). The commands, and their web equivalents, are in [06-dev-loop.md](agents/06-dev-loop.md#0-before-you-start-existing-work-then-a-branch). When related work exists, build on it or talk to its author first.
2. **Make your work findable**, as much as finding others': open a draft pull request early, with the game's name and title ID in its title and the issue it belongs to linked. Comment on the issue that covers it before writing much code.
3. **One branch per effort:** a game's bring-up apart from the general fixes it turns up, each of which gets its own short pull request as soon as it works; optimizations apart from both. Docs go with the change they describe. Merge often.
4. **Know the reach**, and so the tier ([below](#pull-requests)): `python3 scripts/regress.py impact`, or by hand for a design.
5. **Find the root cause** first ([AGENTS.md](../AGENTS.md)); a workaround is the exception, stated as one.

## Changes that apply to every game

A new or removed default, or behaviour switched for every game (Game Mode, a timing source, barriers, a cache size, a thread priority), needs:

- evidence from more than the device it was developed on: a report from another device class, at best the lowest ([test request](#testing-on-the-lowest-devices)), or a statement that only one device was measured and a default that can be changed back per game;
- a switch that restores the old behaviour, listed in [env_switches.tsv](env_switches.tsv), unless the old behaviour was wrong;
- an entry in [decisions.md](decisions.md): the choice, evidence, cost, and what would reverse it. A decision is reversed by a new entry with evidence, not by a commit that flips it back.

## Pull requests

[06-dev-loop.md](agents/06-dev-loop.md#7-before-you-open-a-pull-request) lists the checks by what a change touches. Every pull request also states: one change and its reach; the devices measured on, and whether the lowest was among them; before and after numbers from the same device and scene for a change about speed, memory or power (p95 with the median); what was not checked.

The bar rises with reach and risk. The pull request's labels say most of it; [06-dev-loop.md](agents/06-dev-loop.md#which-tier) has the full guide to judging it:

| Tier | Typically labelled | Shows |
|---|---|---|
| **Narrow** | only `game: …`, `docs`, or a tool few games use | the game played; the tests that apply |
| **Shared** | `host`, `nvn`, `vulkan`, `opengl`, `textures`, `rewriter`, `packing` | `regress.py impact`, and `regress.py check` on the games it names that the author has |
| **Wide or risky** | `shader chain`; or a change to replay ordering and barriers, threads and synchronization, memory layout, a default for every game; or several areas at once | the above on more games (each API and engine family it reaches, and the benchmarks), a test request on the lowest devices, a maintainer's review, and a switch to turn the new path off |

When in doubt, take the higher tier. A wide pull request that can be split is split. One that cannot lands off by default behind a switch, and is turned on in a pull request of its own once played on the benchmarks and the lowest devices.

## Future work

**Regression runs on the base Macs, every day.** Most of the machinery exists: 21 games have a scripted smoke route (`dm/games/<id>/smoke.tsv`), Tears of the Kingdom among them, and `scripts/regress.py check` plays them, comparing frame time, memory, `[RED]` lines and screenshots with a baseline. What is missing is running it where it matters and keeping the result: an 8 GB M1 and a 16 GB M2 that run the benchmarks' routes nightly on `main` and on each wide pull request, a route for Ocarina of Time once it plays, and somewhere to publish the numbers so that "it feels slower" becomes a dated line to bisect.

## Appendix: the Macs people play on

Apple publishes no figures. The Steam Hardware Survey lists Mac chips and is the only regularly updated sample; it leans towards people who play games and undercounts Pro and Max Macs used for work. The figures below are as reported by the linked articles, not checked against Steam's own page.

| Chip class | Share of all Macs on Steam, December 2025 |
|---|---|
| Base: M4 19.60 %, M1 18.06 %, M2 13.08 %, M3 7.31 %, M5 1.09 % | about 59 % |
| Pro: M1 Pro 6.89 %, M4 Pro 5.61 %, M3 Pro 3.64 %, M2 Pro 3.04 % | about 19 % |
| Max: M1 Max 2.61 %, M4 Max 1.95 %, M3 Max 1.45 %, M2 Max 1.31 % | about 7 % |
| Ultra | not listed |
| Intel | 14.36 % |

Of Apple silicon Macs alone, that is about 69 % base, 22 % Pro and 9 % Max. Earlier surveys split similarly: in February 2024, M1 27.36 %, M2 15.56 %, M1 Pro 11.33 %.

Memory: the last Mac breakdown reported, from late 2024, had about 45.2 % with 8 GB and 34.2 % with 16 GB, the rest 24 GB or more. 16 GB became the base memory across the line with the M4 in late 2024 and M4 Macs are now the largest group, so the 8 GB share has likely fallen since; the many M1 to M3 Macs keep it at perhaps a third or more. On Apple silicon the CPU and GPU share memory, so an 8 GB Mac leaves a game roughly 3 to 5 GB.

Sales: CIRP reported the MacBook Pro at 53 % of Macs sold in 2024 and the MacBook Air at 33 %; MacBook Pros also ship with base chips.

Sources: [MacTrast](https://www.mactrast.com/?p=171337), [iDrop News](https://www.idropnews.com/?p=258210), [AppleInsider](https://appleinsider.com/articles/26/01/12/forget-m1-m4-is-the-dominant-apple-silicon-chip-on-steam) (December 2025 chips); [MacRumors forums](https://spy.macrumors.com/threads/in-3-years-50-of-all-computers-capable-of-playing-aaa-games-will-be-macs.2275962/page-66) (February 2024); [Digital Press](https://procyberian.digitalpress.blog/windows-11-overtakes-windows-10-as-the-most-popular-os-among-steam-gamers) (memory, late 2024); [Apple World Today](https://appleworld.today/?p=111254) (CIRP sales). The [Steam Hardware Survey](https://store.steampowered.com/hwsurvey) itself shows the current month.

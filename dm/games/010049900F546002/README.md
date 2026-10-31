# Super Mario Sunshine in Super Mario 3D All-Stars (`010049900F546002`)

Program 2 of the application `010049900F546000`, update 1.1.4 (v393216). SDK 9.5.1. Modules `main`, `subsdk0` and `sdk`.

NERD's Hagi layer, as in Super Mario Galaxy ([010099C022B96000](../010099C022B96000/README.md)):

- the GameCube's PowerPC code is recompiled to arm64 in an LZ4-packed NRO (`data/gekko/MarioSunshine/nx/MarioSunshine.nrolz`), which Hagi unpacks and loads with `nn::ro`;
- it renders with Vulkan, with shaders captured at run time on the Mac.

It plays on the Mac: the title, the opening and Delfino Airstrip.

## What it needs

| Setting | Where | Why |
|---|---|---|
| Packed one program at a time, with `launch_param lang8 f32:1 f32:1` | NSP script `--program 2`, [`game.tsv`](game.tsv) | As Super Mario 64 ([010049900F546001](../010049900F546001/README.md)). |
| `.nrolz` collected | [`nro_collect.py`](../../tools/nro_collect.py) | A `.nrolz` is a big-endian u32 size, then one LZ4 block. The collector unpacks it and matches its SHA-256 against the NRRs, as it does a plain `.nro`. |
| 11.75 GB layout with an 8 GB alias region: `mem_reserve 0x2f0000000`, `mem_alias 0x200000000` | `game.tsv` | See [Memory](#memory). |
| `pad_layout full`, `op_mode handheld` | `game.tsv` | As Galaxy: a 3D pad, and Hagi's pointer is the touch screen in handheld mode. |

## Memory

Hagi reserves two address regions with `nn::os::AllocateAddressRegion`:

- `Memory.Initialize` reserves 4 GB + 2 MB for the GameCube memory;
- its fiber scheduler then reserves 1 GB for the fiber stacks (`main+0x151218`).

The SDK's AddressRegionManager keeps the first `0x3fe00000` of the alias region as its node area, and aligns any region of 1 GB or more to 1 GB. The two regions therefore take `0x80000000-0x180200000` and `0x1c0000000-0x200000000` of the alias region: 8 GB in all.

With a smaller alias region, the 1 GB region fails. The scheduler then never creates the CPU fiber, and switches to an empty context (a crash at `main+0x15ec0c`).

The heap region holds the game's 3.08 GB heap.

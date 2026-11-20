# Catherine: Full Body: select the game's 60 Hz timing mode together with GFD's 60 fps startup rate.
MATCH_CUTS = {'main': [
    {
        'name': 'Catherine 60 fps startup and simulation',
        'functions': [
            {
                # nnMain sets the GFD configuration's rate to 30. GFD uses it for the present interval and animation timing.
                'size': 0xf0,
                'fingerprints': ['966cc9817a3a57178dd0c34b1b2e395c10b0dc2d77fafd0569591870f0dab82b'],
                'cuts': [(0x84, 'e80f1f32', '88078052', 'configure GFD for 60 fps')],
            },
            {
                # Follow the tick callback's call to the timing flag. Its three-instruction getter alone is ambiguous.
                'size': 0xc,
                'fingerprints': ['b97c00f11cf12cbd431bf2acae8edd252d54bff76014491cf77bffccfd5da6cd'],
                'called_by': {
                    'size': 0xa4,
                    'fingerprints': ['102b6acefb704f84502d732696e70949d4c5e87511f67131e43597f56e2bb9ec'],
                    'call': 0x1c,
                },
                'cuts': [(4, '00814d39', '20008052', 'select the one-tick and half-step simulation branches')],
            },
        ],
    },
]}

BINDING = [
    {'what': 'Catherine 60 fps startup and simulation', 'now': 'game', 'goal': 'game', 'purpose': 'frame rate',
     'checked': '1.0.1a startup, tick callback and timing getter verified by disassembly',
     'why': 'GFD already accepts 60 at startup, but the title also selects its own fixed steps. Both sites must match together; no SDK or host setting selects the title’s timing mode.'},
]

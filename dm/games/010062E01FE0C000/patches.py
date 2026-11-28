# Digimon Story Time Stranger selects 30 fps twice in its startup code. Both calls use the MVGL rate setter with a variable time step.
# Select 60 through the same setter. The engine already computes the CPU frame budget and present interval from that rate.
# The separate Frame Rate option handler still passes its selected value to the setter.
rate = 'Time Stranger 60 fps startup'
MATCH_CUTS = {'main': [
    # Base game (v0)
    {'name': rate, 'functions': [
        {'size': 0x15c, 'fingerprints': ['7110cb089d184ee746e07423bb977cffb752eb48c3da1cc8d850b00c2459f9e9'],
         'cuts': [(0x104, 'c0038052', '80078052', 'Startup frame rate 30 to 60')]},
        {'size': 0x244, 'fingerprints': ['dbb2d06034e0749b2c8291532054ef15cb87d2e200fc89a7b991ff44fcb8e46b'],
         'cuts': [(0x114, 'c0038052', '80078052', 'Post-initialization frame rate 30 to 60')]},
    ]},
    # Update 1.2.2 (v262144)
    {'name': rate, 'functions': [
        {'size': 0x164, 'fingerprints': ['fca7db2835ba24b05775832221215fb6140c3905a62c48f4c9362b29241eb826'],
         'cuts': [(0x10c, 'c0038052', '80078052', 'Startup frame rate 30 to 60')]},
        {'size': 0x234, 'fingerprints': ['9b66b33d307e1a75c8664fc3bba2c1d7a0c65d239a2ee036e69778a8d69f0d7d'],
         'cuts': [(0x104, 'c0038052', '80078052', 'Post-initialization frame rate 30 to 60')]},
    ]},
]}

BINDING = [
    {'what': rate, 'now': 'game', 'goal': 'game', 'purpose': 'frame rate',
     'checked': 'both startup callers and the engine limiter verified by disassembly on v0 and 1.2.2. Cyber Sleuth 1.0.3 shares neither caller nor the limiter fingerprint',
     'why': "the game selects 30 fps through MVGL's engine setter. The host already respects its one-refresh present interval"},
]

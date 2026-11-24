# South Park: The Stick of Truth. Select 60 through the game's own frame-rate setter.
MATCH_CUTS = {'main': [
    {
        'name': 'South Park 60 fps target',
        'size': 0x1c8,
        'fingerprints': ['d72e9caa42c81c64e580ac7b4000c0f4821c3ba74206772170869ee5662c4659'],
        # The caller supplies 30 to the setter, which stores the rate and its reciprocal.
        # The main loop uses that reciprocal as its minimum interval and updates with elapsed time.
        'cuts': [(0x19c, 'e00f1f32', '80078052', 'pass 60 fps to the Onyx frame-rate setter')],
    },
]}

BINDING = [
    {'what': 'South Park 60 fps target', 'now': 'game', 'goal': 'game', 'purpose': 'frame rate',
     'checked': '1.0.0 and 1.01 match the complete caller and setter',
     'why': 'The title passes 30 to its own Onyx frame-rate setter while requesting NVN present interval 1. The matched caller selects 60 through the same setter.'},
]

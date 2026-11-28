rate = 'Storm 3 Full Burst native 60 fps'

# The application passes 30 to CyberConnect2's rate setter. The setter uses 60 / rate
# for both the vertical blank wait and the engine's frame duration. Select 60 there.
# The same startup function and setter call match in 1.0.0 and 1.0.1.
MATCH_CUTS = {'main': [
    {'name': rate, 'size': 0x5bc,
     'fingerprints': ['a83a2ce92f150df53ae01bd660df941eb60db33a14d3b9eb5ba2aaa877cbaed9'],
     'cuts': [(0xf4, '810e40b9', '81078052', 'Pass 60 fps to the startup rate setter')]},
]}

BINDING = [
    {'what': rate, 'now': 'game', 'goal': 'game', 'purpose': 'frame rate',
     'checked': 'The same function match locates the startup call on 1.0.0 and 1.0.1',
     'why': 'The title chooses the frame rate in its own startup code. The host already supplies 60 Hz display events.'},
]

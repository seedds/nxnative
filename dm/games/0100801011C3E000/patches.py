"""Select Strikers' native 60 Hz timing mode and its matching KTGL present interval."""

# Strikers counts gameplay in 1/60 s units. Its 30 Hz mode adds two units per
# update and supplies 30 Hz to float-based timers. Setting the native rate here
# also selects the KIDS task's own wait and time step. Keep the startup and scene
# reset writers together so a transition cannot restore half-rate timing.
_FULL_RATE = 'select the 60 Hz timing mode'
_RATE = 'set the integer update rate to 60 Hz'
_STEP = 'advance gameplay counters by one 60 Hz unit per update'
_FLOAT_RATE = 'set the float update rate to 60 Hz'

MATCH_CUTS = {'main': [
    {'name': '60 Hz gameplay timing and presentation', 'functions': [
        # Startup settings.
        {'size': 764,
         'fingerprints': ['206ac82a5be9e213466e92b3a25d797fecf1966362098c944c9530ef6f38da8a'],
         'cuts': [
             (0x14, 'e9030032', '09008052', _FULL_RATE),
             (0x20, 'e90f1f32', '89078052', _RATE),
             (0x28, 'eb031f32', '2b008052', _STEP),
             (0x34, '093ea852', '094ea852', _FLOAT_RATE),
         ]},
        # Scene-state rate writers. The restoration path computes both rates
        # from the saved step, so it follows the mode without a cut.
        {'size': 2968,
         'fingerprints': ['ac5664028e9946e46b7463a5427ad6602391560ff8ef1b28367b194609ab31f2'],
         'cuts': [
             (0x2e4, 'e9030032', '09008052', _FULL_RATE),
             (0x2ec, 'e8031f32', '28008052', _STEP),
             (0x2f8, 'e90f1f32', '89078052', _RATE),
             (0x304, '093ea852', '094ea852', _FLOAT_RATE),
         ]},
        {'size': 556,
         'fingerprints': ['b54fe322e129746924ddcc568cdbd1005c8f68852b1ab07aff9b156485f79e10'],
         'cuts': [
             (0x14, 'e9030032', '09008052', _FULL_RATE),
             (0x20, 'e9031f32', '29008052', _STEP),
             (0x2c, 'e90f1f32', '89078052', _RATE),
             (0x38, '093ea852', '094ea852', _FLOAT_RATE),
         ]},
        {'size': 18004,
         'fingerprints': ['a97ecd278ee46a3202f84ea783505068deaeee51e50ef31286adcf6aede87a84'],
         'cuts': [
             (0x4f0, '3f090071', '3f050071', 'compare the current step against the one-unit scene default'),
             (0x2dcc, 'e9031f32', '29008052', _STEP),
             (0x2dd4, 'eb0f1f32', '8b078052', _RATE),
             (0x2de4, '0a3ea852', '0a4ea852', _FLOAT_RATE),
         ]},
        {'size': 1040,
         'fingerprints': ['688557f846ba9e7f2cdadbbf8df06d8496e64b3c6813c47ddbddabdb41c1b517'],
         'cuts': [
             (0x184, 'e9030032', '09008052', _FULL_RATE),
             (0x190, 'e90f1f32', '89078052', _RATE),
             (0x198, 'eb031f32', '2b008052', _STEP),
             (0x1a4, '093ea852', '094ea852', _FLOAT_RATE),
         ]},
        {'size': 2112,
         'fingerprints': ['1e7cb6b7d1b4a6bab7198963a6932b464c52af060a48569de5c27789fbff7e71'],
         'cuts': [
             (0x48, 'ea031f32', '2a008052', _STEP),
             (0x54, 'ea0f1f32', '8a078052', _RATE),
             (0x60, '0a3ea852', '0a4ea852', _FLOAT_RATE),
         ]},
        # The KTGL device uses its startup interval for its refresh-rate
        # estimate and its NVN present requests.
        {'size': 592,
         'fingerprints': ['5cd66bcfa602bd682eb9b63684c6ecc50dea18f1f3ddb712c03bf7e4d11786b7'],
         'cuts': [(0x2c, '4800c0f2', '2800c0f2', 'initialize the KTGL device with a one-vsync interval')]},
    ]},
]}

BINDING = [
    {'what': '60 Hz gameplay timing and presentation', 'purpose': 'frame rate',
     'now': 'game', 'goal': 'game',
     'why': 'Strikers sets its own integer and float update rates, counter step and half-rate mode at startup and on scene changes. '
            'Changing the host or KTGL interval alone leaves those writers at 30 Hz.',
     'checked': '1.0.0 main: all seven functions, original instructions, and the native 60 Hz mode on Mac'},
]

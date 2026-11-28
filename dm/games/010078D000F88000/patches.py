full_rate = 'Use Xenoverse 2 full-rate timing'
city_rate = 'Keep the incoming base rate in Conton City'


def full_rate_rule(fingerprint, store, store_zero):
    # The setter's flag halves the simulation rate when the scene requests 60 Hz.
    # Pass zero to the existing comparison, store and renderer reconfiguration.
    # Scenes that request 30 Hz directly use the separate base-rate setter.
    return {
        'name': full_rate,
        'size': 0xec,
        'fingerprints': [fingerprint],
        'cuts': [
            (0x1c, '3f01006b', '3f011f6b', 'Compare the current half-rate flag with zero'),
            (0x24, store, store_zero, 'Store zero as the half-rate flag'),
            (0x30, 'f403002a', 'f4031f2a', 'Configure the renderer for the full base rate'),
        ],
    }


MATCH_CUTS = {'main': [
    # 1.25.02
    full_rate_rule('2b0e190f2e46c38b2fd6d0e03b53a5b9d69d2b243bd3ba896ec6bc1c8dc101db',
                   '009d00b9', '1f9d00b9'),
    # 1.26.01
    full_rate_rule('2b11a73068cc922af9815d24d458d89296fcee6529b1c0ecda30eb9be9b0da5c',
                   '00dd06b9', '1fdd06b9'),
    # The city constructor saves the incoming base rate from s0 and then calls
    # the integer flag getter, which leaves s0 intact. Pass the saved rate
    # through to the base-rate setter.
    {'name': city_rate, 'size': 0x6c,
     'fingerprints': [
         '1a3bb83cb1689cffbfa8ca92e1f4265353077ee548ef18fb24bec8d343ea86d7',  # 1.25.02
         '1ad9844c24558a4718e336a121425ad886f3cf53e9f83981ae76b5a490e9f1ad',  # 1.26.01
     ],
     'cuts': [(0x44, '00d0271e', '1f2003d5', city_rate)]},
]}

BINDING = [
    {'what': full_rate, 'now': 'game', 'goal': 'game', 'purpose': 'frame rate',
     'checked': 'half-rate setter and base-rate selection disassembled in 1.25.02 and 1.26.01',
     'why': 'the game chooses the half-rate mode for its simulation and rendering together. The host and SDK see the present interval independently of that choice.'},
    {'what': city_rate, 'now': 'game', 'goal': 'game', 'purpose': 'frame rate',
     'checked': 'Conton City entry traced in 1.26.01 and its constructor and rate getters disassembled in 1.25.02 and 1.26.01',
     'why': 'the game owns the city lifecycle and selects the 30 Hz rate in its city constructor.'},
]

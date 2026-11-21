"""Kirby's Dream Buffet: order the dependent module initialization jobs."""
import struct

from nso_asm import ADDI, ADRP, B, BEQ, BL, BLR, CBNZW, LDP_POST, LDRX, LDUR, STP_PRE, STRX

# Modules.Initialize queues BaseSystem, Info and Decor together. Info's
# RewardGet.OnSetup caches the GamePauseManager before BaseSystem creates it:
# OnUpdate then reads __bf_IsPause through null (main+0x7f23ec on 1.0.0).
# Complete the BaseSystem job through the game's own queue, before its caller
# submits Info and Decor. The remaining jobs still run in parallel. The native
# bindings and task runner are matched as complete functions; the runner proves
# the instance/type-name layout. The extra wait also requires the
# exact class and method names, since this queue serves unrelated frame work.


def initialize_base_system(ctx):
    """Complete BaseSystem before the caller queues dependent Info setup."""
    execute = ctx.others.get('execute and wait')
    if execute is None or ctx.others.get('task runner') is None:
        return  # The matcher already reported the missing companion; install nothing.
    add = ctx.base
    ctx.site(add, 'ff0302d1')       # sub sp,sp,#0x80
    ctx.site(execute, 'fd7bbda9')   # stp x29,x30,[sp,#-0x30]!
    start, words, labels = ctx.here(), [], {}

    def label(name):
        labels[name] = start + 4 * len(words)

    def address(register, name):
        words.append(lambda pc: ADRP(register, pc, labels[name]))
        words.append(lambda pc: ADDI(register, register, labels[name] & 0xfff))

    def match_string(name):
        address(10, name)
        label(name + ' loop')
        words.extend((0x3840152b, 0x3840154c, 0x6b0c017f))  # ldrb w11,[x9],#1; ldrb w12,[x10],#1; cmp w11,w12
        words.append(lambda pc: BEQ(pc, labels['return']) | 1)  # b.ne return
        words.append(lambda pc: CBNZW(11, pc, labels[name + ' loop']))

    # Keep the original native-call context across Add, whose x0 result is not
    # the context. Its argument slots remain {queue, instance, method name}.
    words.extend((STP_PRE(29, 30, 31, -0x20), 0x910003fd, STRX(0, 31, 2)))
    words.append(lambda pc: BL(pc, labels['original']))
    words.extend((LDRX(0, 31, 2), LDRX(8, 0), LDRX(8, 8, 0x88 // 8), LDRX(9, 8, 2)))
    # Null names/instances are not the Modules task; compare the full names,
    # including NUL, so unrelated tasks and similarly named classes keep Add.
    words.append(lambda pc: CBNZW(9, pc, labels['return']) ^ 0x81000000)  # cbz x9,return
    match_string('method')
    words.append(LDRX(9, 8, 1))
    words.append(lambda pc: CBNZW(9, pc, labels['return']) ^ 0x81000000)  # cbz x9,return
    words.extend((LDUR(9, 9, -8), LDRX(9, 9, 1)))
    match_string('class')
    words.append(LDRX(0, 31, 2))
    words.append(lambda pc: ADRP(16, pc, execute))
    words.extend((ADDI(16, 16, execute & 0xfff), BLR(16)))
    label('return')
    words.extend((LDP_POST(29, 30, 31, 0x20), 0xd65f03c0))
    label('original')
    words.append(0xd10203ff)  # original sub sp,sp,#0x80
    words.append(lambda pc: B(pc, add + 4))
    for name, value in [('method', b'InitializeBaseSystem\0'), ('class', b'Scn.Step.Modules\0')]:
        label(name)
        value += b'\0' * (-len(value) % 4)
        words.extend(struct.unpack('<' + 'I' * (len(value) // 4), value))
    for word in words:
        ctx.emit(word(ctx.here()) if callable(word) else word)
    ctx.put(add, [B(add, start)], 'Modules.InitializeBaseSystem: finish its queued initialization before Info/Decor are added')


MATCH_STUBS = {'main': [
    {'name': 'BaseSystem initialization precedes Info setup', 'size': 248,
     'fingerprints': ['0904214804ff466cc9a0d9911656bb8888f89d08a9b5819b4709f696b8bdceca'],
     'stub': initialize_base_system,
     'others': {
         'execute and wait': {'size': 208, 'fingerprints': ['436e2422d30d906519630af2c9f3da3e7019c30f23d692d5246b39d5f6217494']},
         'task runner': {'size': 672, 'fingerprints': ['6d177ae833a2260661809aeee11c69454659857235deed9000bfc44a9ec33913']},
     }},
]}

BINDING = [
    {'what': 'BaseSystem initialization before dependent Info setup', 'now': 'game', 'goal': 'game', 'purpose': 'race',
     'why': "Scn.Step.Modules queues BaseSystem and Info setup together, but RewardGet.OnSetup caches the GamePauseManager that BaseSystem creates",
     'checked': 'Dream Buffet 1.0.0: complete Add/ExecuteAndWait/task-runner functions, Basil initialization call graph, and null-cache-before-source-write runtime trace'},
]

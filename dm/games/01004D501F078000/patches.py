# The Lord of the Rings: War in the North - Legacy Edition
#
# Game-specific patches applied by dm/tools/patch_nso.py.

CUTS = {
    # 1.0.2 (v131072), main.
    #
    # The job system initializes its worker event with manual-clear mode, but
    # workers repeatedly wait on this event without explicitly clearing it.
    # After the first signal, WaitEvent therefore keeps returning immediately
    # and the Job kernel thread spins instead of sleeping.
    #
    # Use auto-clear mode so one successful wait consumes the signal and the
    # worker returns to its normal condition-variable wait when idle.
    #
    # Observed on Mac:
    #   before: Job kernel CPU 30.00 s over 30.2 s
    #   after:  Job kernel CPU  0.05 s over 30.2 s
    'd70341137daa544c7730db2e4bd728a8': [
        (0x25ff98, 'e2031f2a', '22008052',
         'job worker event: manual-clear -> auto-clear'),
    ],
}

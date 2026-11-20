"""The save policy of a host run that is not the player's own: a regression or benchmark run (regress.py, bench.py), the NSP script's tools and pipeline prebuild.

Such a run plays on a scratch save, and cloud saves are off whatever the shell or an environment file says: the cloud's file is named by title ID (dm/host/save_sync.c), so a run
that synced pulled the player's cloud save into its route (a save=new route started from the player's progress) and sent the run's progress over it (the review's F2). The
launchers' answer to which save to keep (DM_SAVE_SYNC_CHOICE) is the player's and means nothing with sync off, so it goes too. Callers apply cloud_off last, after every override
they merge, so none can undo it; an explicit option of their own that turns sync on is refused, not silently dropped. Cloud sync itself is tested against a temporary cloud of
its own (dm/host/tests/t_save_sync.c, t_save_sync_metadata.c), not through these runs."""


class CloudSync(ValueError):
    """An option of the caller's own asked a run that is not the player's to sync its save with a cloud"""


def asked(pairs):
    """The DM_SAVE_SYNC value KEY=VALUE options set (the last one wins, as when they are merged in order), None when none sets it"""
    v = None
    for kv in pairs:
        k, _, val = kv.partition('=')
        if k == 'DM_SAVE_SYNC': v = val
    return v


def cloud_off(env, ask=None):
    """env (a dict, changed in place and returned) with cloud saves off: DM_SAVE_SYNC=0, no DM_SAVE_SYNC_CHOICE. ask: the DM_SAVE_SYNC value the caller's own options set
    (asked(), a benchmark variant), None when they set none; one that turns sync on ('' and 0 do not) raises CloudSync"""
    if ask is not None and ask not in ('', '0'):
        raise CloudSync(f"DM_SAVE_SYNC={ask}: a run that is not the player's does not sync its save with a cloud (dm/host/tests/t_save_sync.c covers cloud saves)")
    env['DM_SAVE_SYNC'] = '0'
    env.pop('DM_SAVE_SYNC_CHOICE', None)
    return env

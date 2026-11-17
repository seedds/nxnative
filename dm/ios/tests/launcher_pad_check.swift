// Play and Quit from a game controller in the Mac settings window (dm/mac/Launcher.swift LauncherPad), run by test_launcher_storage.py: the decisions, fed button
// samples as the window's timer reads them, without a controller.
func checkLauncherPad() -> Int {
    var failed = 0
    func expect(_ c: Bool, _ what: String) { print((c ? "PASS " : "FAIL ") + what); if !c { failed += 1 } }
    typealias Sample = (a: Bool, b: Bool, window: Bool, play: Bool)
    func run(_ samples: [Sample]) -> [LauncherPad.Action?] {
        var pad = LauncherPad()
        return samples.map { pad.step(a: $0.a, b: $0.b, window: $0.window, playEnabled: $0.play) }
    }
    let idle: Sample = (false, false, true, true), a: Sample = (true, false, true, true), b: Sample = (false, true, true, true)
    func modal(_ s: Sample) -> Sample { (s.a, s.b, false, s.play) }
    func checking(_ s: Sample) -> Sample { (s.a, s.b, s.window, false) }

    expect(run([idle, a, a, idle]) == [nil, nil, nil, .play], "A is Play on its release, not its press: the button is up when the host starts")
    expect(run([idle, b, idle]) == [nil, nil, .quit], "B is Quit on its release")
    expect(run([a, a, idle, idle]) == [nil, nil, nil, nil], "an A already held when the window takes input is no Play")
    expect(run([a, idle, a, idle]) == [nil, nil, nil, .play], "after such an A is let go, the next press and release is Play")
    expect(run([idle, a, modal(a), modal(idle), idle]) == [nil, nil, nil, nil, nil], "an A pressed before an alert opens and let go in it is no Play")
    expect(run([idle, modal(a), modal(idle), idle]) == [nil, nil, nil, nil], "an A pressed and let go while an alert is up answers nothing")
    expect(run([idle, modal(a), a, idle]) == [nil, nil, nil, nil], "an A pressed in an alert and held after it closes is no Play")
    expect(run([idle, modal(idle), idle, a, idle]) == [nil, nil, nil, nil, .play], "after an alert closes, a new press is Play")
    expect(run([idle, modal(idle), a, idle]) == [nil, nil, nil, nil], "an A seen down at the first sample after an alert closes is no Play (it may be the alert's)")
    expect(run([idle, checking(a), checking(idle), idle]) == [nil, nil, nil, nil], "A does nothing while Play is disabled (the cloud-save check)")
    expect(run([idle, checking(b), checking(idle)]) == [nil, nil, .quit], "B quits during the cloud-save check, as the Quit button does")
    expect(run([idle, a, checking(a), checking(idle)]) == [nil, nil, nil, nil], "an A whose Play became disabled before its release is dropped")
    expect(run([idle, (true, true, true, true), idle]) == [nil, nil, .play], "A and B let go together: Play")
    expect(run([idle, a, (true, true, true, true), b, idle]) == [nil, nil, nil, .play, .quit], "each button is a click of its own")
    return failed
}

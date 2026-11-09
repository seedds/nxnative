// Extend the upstream card without replacing its count, actions or appearance.
#pragma once
#include "home_quit_mac.h"
#include "ctrl_prompt.h"

static NSView *controller_join_card(NSRect bounds, const ctrl_prompt *state, NSString *a, NSString *b, id target, SEL configure) {
    const char *env = getenv("DM_CTRL_PROMPT_TEXT"); char text[CP_TEXTS][256];
    for (int i=0;i<CP_TEXTS;i++) cp_text(env,i,state,text[i],sizeof text[i]);
    NSView *view=hq_card_view(bounds,@(text[CP_TEXT_TITLE]),[NSString stringWithFormat:@"%s\n%s",text[CP_TEXT_HINT],text[CP_TEXT_COUNT]],
        @(text[CP_TEXT_DONE]),@(text[CP_TEXT_CANCEL]),a,b,cp_can_confirm(state));
    NSButton *button=[NSButton buttonWithTitle:NSLocalizedString(@"Configure controllers…",nil) target:target action:configure];
    button.identifier=@"controller.join.configure";
    button.bezelStyle=NSBezelStyleRounded;
    [(NSStackView *)view.subviews.firstObject addArrangedSubview:button];
    return view;
}

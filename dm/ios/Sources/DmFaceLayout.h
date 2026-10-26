// Display and hit-test geometry only. Native face-button bits never change.
#pragma once
typedef struct { int skill; } dm_face_layout;
typedef struct { int x, y; } dm_face_offset;

static inline int dm_face_layout_update(dm_face_layout *layout, int requested, int face_owned) {
    requested = requested != 0;
    // A touch can own the diamond while resting between its keys. Wait for
    // every such touch to end, not merely for the currently pressed bits to clear.
    if (face_owned || layout->skill == requested) return 0;
    layout->skill = requested;
    return 1;
}

static inline dm_face_offset dm_face_layout_offset(const dm_face_layout *layout, unsigned button) {
    // A/B/X/Y. Positive y is downward in UIKit. During explicit move selection
    // the frontend matches the original game's A right/B bottom/X top/Y left.
    static const dm_face_offset normal[4] = {{0, 1}, {1, 0}, {-1, 0}, {0, -1}};
    static const dm_face_offset skill[4] = {{1, 0}, {0, 1}, {0, -1}, {-1, 0}};
    return button < 4 ? (layout->skill ? skill[button] : normal[button]) : (dm_face_offset){0, 0};
}

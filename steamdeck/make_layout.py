#!/usr/bin/env python3
"""
Makes controller_neptune_openxcom3d.vdf, the Steam Deck controller layout for OpenXcom3D, from
Valve's own "Keyboard (WASD) and Mouse" template for the Deck (controller_neptune_wasd.vdf, in
Steam's controller_base/templates): the same file, groups and settings, with OpenXcom3D's
bindings and its own title. Usage: make_layout.py VALVE_TEMPLATE OUTPUT
"""
import re
import sys

# OpenXcom3D's layout: the group's id in Valve's template, the input, and its binding.
BINDINGS = [
    # the face buttons (four_buttons)
    ("0", "button_a", "key_press SPACE"),          # jump / vault
    ("0", "button_b", "key_press R"),              # reload
    ("0", "button_x", "key_press F"),              # next fire mode
    ("0", "button_y", "key_press I"),              # inventory
    # the left trackpad, clicked (dpad): the fire modes and the door
    ("1", "dpad_north", "key_press Z"),            # snap shot
    ("1", "dpad_east", "key_press X"),             # aimed shot
    ("1", "dpad_south", "key_press V"),            # auto shot
    ("1", "dpad_west", "key_press O"),             # open the door ahead
    # the right trackpad (absolute_mouse): look / the cursor, click = left button (as Valve's)
    ("2", "click", "mouse_button LEFT"),
    # the left stick (dpad): walk and step sideways, click = kneel
    ("3", "dpad_north", "key_press W"),
    ("3", "dpad_south", "key_press S"),
    ("3", "dpad_east", "key_press D"),
    ("3", "dpad_west", "key_press A"),
    ("3", "click", "key_press K"),
    # the triggers: right = left button (fire, click), left = right button (cancel, hold to look)
    ("4", "edge", "mouse_button RIGHT"),
    ("5", "edge", "mouse_button LEFT"),
    # the d-pad: up and down (flight, lifts), next soldier, switch hand
    ("7", "dpad_north", "key_press PAGE_UP"),
    ("7", "dpad_south", "key_press PAGE_DOWN"),
    ("7", "dpad_east", "key_press TAB"),
    ("7", "dpad_west", "key_press G"),
    # the right stick (joystick_mouse): a finer look, click = left button (as Valve's)
    ("9", "click", "mouse_button LEFT"),
    # the switches: View = the game's menu, Menu = classic HUD, L1 = mouse look / free cursor,
    # R1 = isometric / 3D, back buttons: L4 light, L5 night vision, R4 flare, R5 end turn
    ("6", "button_escape", "key_press ESCAPE"),
    ("6", "button_menu", "key_press H"),
    ("6", "left_bumper", "key_press C"),
    ("6", "right_bumper", "key_press F8"),
    ("6", "button_back_left_upper", "key_press L"),
    ("6", "button_back_left", "key_press N"),
    ("6", "button_back_right_upper", "key_press T"),
    ("6", "button_back_right", "key_press BACKSPACE"),
]

TITLE = "OpenXcom3D"
DESCRIPTION = ("OpenXcom3D's first-person battlescape: left stick walks (WASD), the right trackpad looks "
               "(mouse), R2 fires, L2 cancels. L1 switches mouse look and the free cursor, R1 the "
               "isometric map and 3D. A jump, B reload, X fire mode, Y inventory; d-pad up/down "
               "flight and lifts, right next soldier, left switch hand; left trackpad snap, aimed, "
               "auto, door; L4 light, L5 night vision, R4 flare, R5 end turn.")


def main():
    template, output = sys.argv[1], sys.argv[2]
    text = open(template, encoding="utf-8-sig").read()

    # The title, and one language (the template's others describe Valve's layout).
    text = text.replace('"game" "WASD with Mouse"', '"game" "%s"' % TITLE, 1)
    start = text.index('\t"localization"')
    end = text.index('\t"group"')
    text = text[:start] + ('\t"localization"\n\t{\n\t\t"english"\n\t\t{\n\t\t\t"Title" "%s"\n\t\t\t"Description" "%s"\n'
                           '\t\t}\n\t}\n' % (TITLE, DESCRIPTION)) + text[end:]

    # The groups, each with its bindings set.
    parts = re.split(r'(?=\n\t"group"\n)', text)
    for index, part in enumerate(parts):
        found = re.search(r'\n\t\t"id"\t\t"(\d+)"', part)
        if not part.startswith('\n\t"group"') or not found:
            continue
        group = found.group(1)
        for wanted_group, name, binding in BINDINGS:
            if wanted_group != group:
                continue
            at = part.index('\t\t\t"%s"\n' % name)
            bindings = part.index('"bindings"', at)
            open_brace = part.index("{", bindings)
            close_brace = part.index("}", open_brace)
            indent = part[part.rindex("\n", 0, bindings) + 1:bindings]
            part = (part[:open_brace + 1] + '\n%s\t"binding"\t\t"%s"\n%s' % (indent, binding, indent) +
                    part[close_brace:])
        if group == "1":
            # the left trackpad acts only when clicked, so a resting thumb fires no mode change
            part = part.replace('"edge_binding_radius"\t\t"24996"', '"requires_click"\t\t"1"\n\t\t\t"edge_binding_radius"\t\t"24996"', 1)
        parts[index] = part
    text = "".join(parts)

    # Every binding asked for is in, once.
    for group, name, binding in BINDINGS:
        assert text.count('"%s"' % binding) >= 1, binding
    # (with the template's own line ends, CRLF)
    open(output, "w", encoding="utf-8-sig", newline="\r\n").write(text)


if __name__ == "__main__":
    main()

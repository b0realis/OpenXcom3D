#!/usr/bin/env python3
"""
Makes controller_neptune_openxcom3d.vdf, the Steam Deck controller layout for OpenXcom3D, from
Valve's own "Keyboard (WASD) and Mouse" template for the Deck (controller_neptune_wasd.vdf, in
Steam's controller_base/templates): the same file, groups and settings, with OpenXcom3D's
bindings and its own title. Usage: make_layout.py VALVE_TEMPLATE OUTPUT
"""
import re
import sys

# OpenXcom3D's layout: the group's id in Valve's template, the input, its binding (or several,
# pressed together, as a tuple), and what a long press of it does (a fourth entry, for some).
# With a long press on an input, Steam sends its
# ordinary press only when it's let go before LONG_PRESS_MS (the press is "interruptable" by
# default), so long presses go on inputs that are tapped, never on the ones held (walking, the
# lean keys).
BINDINGS = [
    # the face buttons (four_buttons)
    ("0", "button_a", "key_press SPACE"),          # jump / vault
    ("0", "button_b", "key_press R"),              # reload
    ("0", "button_x", "key_press J"),              # next fire mode
    ("0", "button_y", "key_press I"),              # inventory
    # the left trackpad, clicked (dpad): the fire modes and the door
    ("1", "dpad_north", "key_press Z"),            # snap shot
    ("1", "dpad_east", "key_press X", "key_press PERIOD"),  # aimed shot; held: snap turn right
    ("1", "dpad_south", "key_press V"),            # auto shot
    ("1", "dpad_west", "key_press O", "key_press COMMA"),   # open the door ahead; held: snap turn left
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
    # the d-pad: up and down (flight, lifts), next soldier (held: the flashlight), switch hand
    ("7", "dpad_north", "key_press PAGE_UP"),
    ("7", "dpad_south", "key_press PAGE_DOWN"),
    ("7", "dpad_east", "key_press TAB", "key_press F"),     # held: the flashlight on/off
    ("7", "dpad_west", "key_press G", "key_press Y"),       # held: akimbo on/off
    # the right stick (joystick_mouse): a finer look, click = left button (as Valve's)
    ("9", "click", "mouse_button LEFT"),
    # the switches: Menu (button_escape, the Deck's right one, Valve's Start) = the game's menu,
    # View (button_menu, the left one, Valve's Select) = classic HUD (held: the 3D HUD on/off),
    # L1 = mouse look / free cursor, R1 = isometric / 3D, back buttons: L4 / R4 held lean left /
    # right (setting "Lean": the lean key, Left Shift, with a strafe key, so Q and E stay the
    # hands), L5 night vision (held: the suit light, the game's personal light), R5 a flare (held:
    # end turn, so a press a little short of a long one never ends the turn)
    ("6", "button_escape", "key_press ESCAPE"),
    ("6", "button_menu", "key_press H", "key_press P"),
    ("6", "left_bumper", "key_press C"),
    ("6", "right_bumper", "key_press F8"),
    ("6", "button_back_left_upper", ("key_press LEFT_SHIFT", "key_press A")),
    ("6", "button_back_left", "key_press N", "key_press L"),
    ("6", "button_back_right_upper", ("key_press LEFT_SHIFT", "key_press D")),
    ("6", "button_back_right", "key_press T", "key_press BACKSPACE"),
]
# How long a press is held to count as a long press, in milliseconds.
LONG_PRESS_MS = 400

TITLE = "OpenXcom3D"
DESCRIPTION = ("OpenXcom3D's first-person battlescape: left stick walks (WASD), the right trackpad looks "
               "(mouse), R2 fires, L2 cancels. L1 switches mouse look and the free cursor, R1 the "
               "isometric map and 3D. A jump, B reload, X fire mode, Y inventory; d-pad up/down "
               "flight and lifts, right next soldier (held: flashlight), left switch hand (held: akimbo); left trackpad "
               "snap, aimed, auto, door (left/right held: snap turns); L4/R4 lean (setting Lean), "
               "L5 night vision (held: suit light), R5 flare (held: end turn); View classic HUD (held: 3D HUD), Menu the game's menu.")


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
        for wanted_group, name, binding, *long_press in BINDINGS:
            if wanted_group != group:
                continue
            at = part.index('\t\t\t"%s"\n' % name)
            bindings = part.index('"bindings"', at)
            open_brace = part.index("{", bindings)
            close_brace = part.index("}", open_brace)
            indent = part[part.rindex("\n", 0, bindings) + 1:bindings]
            lines = "".join('\n%s\t"binding"\t\t"%s"' % (indent, one) for one in (binding if isinstance(binding, tuple) else (binding,)))
            part = part[:open_brace + 1] + lines + '\n' + indent + part[close_brace:]
            if long_press:
                # A Long_Press activator after the Full_Press one, at its indent, with its own
                # binding and the time it takes.
                full_press = part.index('"Full_Press"', at)
                outer = part[part.rindex("\n", 0, full_press) + 1:full_press]
                end = part.index("\n%s}" % outer, full_press) + len(outer) + 2
                part = (part[:end] + '\n%s"Long_Press"\n%s{\n%s\t"bindings"\n%s\t{\n%s\t\t"binding"\t\t"%s"\n%s\t}\n'
                        '%s\t"settings"\n%s\t{\n%s\t\t"long_press_time"\t\t"%d"\n%s\t}\n%s}'
                        % (outer, outer, outer, outer, outer, long_press[0], outer, outer, outer, outer, LONG_PRESS_MS, outer, outer)
                        + part[end:])
        if group == "1":
            # the left trackpad acts only when clicked, so a resting thumb fires no mode change
            part = part.replace('"edge_binding_radius"\t\t"24996"', '"requires_click"\t\t"1"\n\t\t\t"edge_binding_radius"\t\t"24996"', 1)
        parts[index] = part
    text = "".join(parts)

    # Every binding asked for is in, once.
    for group, name, binding, *long_press in BINDINGS:
        for one in (binding if isinstance(binding, tuple) else (binding,)):
            assert text.count('"%s"' % one) >= 1, one
        for long_binding in long_press:
            assert text.count('"%s"' % long_binding) == 1, long_binding
    # (with the template's own line ends, CRLF)
    open(output, "w", encoding="utf-8-sig", newline="\r\n").write(text)


if __name__ == "__main__":
    main()

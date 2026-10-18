# Tongs

Tongs lets scripts and agents control and observe a running Europa
Universalis IV through commands with a verifiable answer.

## Language

**Game call**:
Game code that Tongs runs on the game's main thread on a client's request;
a console command is the first kind.
_Avoid_: function call, invoke, DOM call

**Widget**:
One element of the game's interface (a window, button, text, icon), named as
in the game's `.gui` files.
_Avoid_: DOM node, element, control

**Widget tree**:
The widgets open in the game's interface at one moment, each with its
children.
_Avoid_: DOM, UI tree

**Selector**:
A text that names one or more widgets in the widget tree.
_Avoid_: query, path, XPath

**Input**:
A key, text or mouse event that Tongs gives the game the way a player's
keyboard and mouse would.
_Avoid_: injection, synthetic event

# Tongs

Tongs lets scripts and agents control and observe a running Europa
Universalis IV through commands with a verifiable answer.

## Language

**Game call**:
Game code that Tongs runs on the game's main thread on a client's request and
that changes or may change the state of the game; a console command is the
first kind.
_Avoid_: function call, invoke, DOM call

**Main-thread read**:
A read of the game that Tongs does on the game's main thread and that changes
nothing, such as reading the widget tree; not a game call.
_Avoid_: safe call, read call

**Marked run**:
A stretch between two dates measured by Tongs during which at least one game
call or input from any client happened.
_Avoid_: tainted run, dirty run

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

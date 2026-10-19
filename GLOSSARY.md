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

**Outer frame**:
One frame of the game's main loop, as opposed to a frame the game draws from
inside a game update or a save load; game calls and main-thread reads run only
after outer frames.
_Avoid_: tick, frame (unqualified)

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
_Avoid_: query, XPath

**Widget path**:
The selector that names exactly one widget by the names of its ancestors,
with a position where siblings share a name; Tongs gives it in every answer.
_Avoid_: canonical selector, full name

**Event**:
A situation from the game's scripts that the game shows in an event window
with options for the player to choose; only the game's own events.
_Avoid_: pop-up (for the event itself), notification

**Notification**:
A message Tongs sends to a subscribed client on its own, not as the answer to
a request.
_Avoid_: event, push, callback

**Subscription**:
A pipe connection that has sent `subscribe` and carries notifications of the
kinds it named, through its own bounded queue, until `unsubscribe`.
_Avoid_: listener, channel, watch

**Input**:
A key press, typed text or mouse action that Tongs gives the game the way a player's
keyboard and mouse would.
_Avoid_: injection, synthetic event

**Let your scripts and AI agents play EU4 through the front door, not through screenshots. A smith never holds hot iron bare-handed.**

![](https://i.imgur.com/Uf8Z21n.png)

## Overview
Tongs is for people who automate EU4: mod testers, tool makers and anyone who wants an AI agent to look at a game and press its buttons. While the game runs, a program on your own computer can ask Tongs what is going on (the date, the speed, the open windows and events, a screenshot) and, if you allow it, pause, change speed, press keys and click buttons by name, just as a player would.

Other Anvil plugins can read the game's interface through Tongs too, even while its pipe is switched off.

On its own Tongs does nothing you can see. It waits until you switch it on, and it only listens to programs on your own computer.

![](https://i.imgur.com/Uf8Z21n.png)

## Before you subscribe
- **[Anvil](https://anvil.happydwarf.dev/) 1.3 or newer** must be installed in the game folder. Get it from [anvil.happydwarf.dev](https://anvil.happydwarf.dev/) and press Download. The Workshop can't install it for you: a dwarf has to carry it in by hand, once.
- **EU4 1.37.5** (Steam, Windows). On any other version Tongs stays out of the way and the game starts as usual.
- **Something to talk to it:** a script or an agent. The [GitHub page](https://github.com/happy-dwarf-team/tongs) has ready clients for PowerShell and Python and an MCP server, which lets Claude Code and other AI agents use Tongs as a set of tools.

![](https://i.imgur.com/Uf8Z21n.png)

## Getting it running
1. Subscribe and enable Tongs in your playset.
2. Start the game. Anvil will tell you a new plugin is waiting: press **Ctrl+Shift+F12**, open **Mods** and approve Tongs. You can also approve it before playing, in Anvil Manager.
3. Switch Tongs on in the same panel or in Anvil Manager (it starts switched off), allow keyboard and mouse if your script needs them, and restart.

Anvil asks again after every update. Until you approve, the version you trusted before keeps working.

![](https://i.imgur.com/Uf8Z21n.png)

## What it doesn't touch
- No game files: the files the game checks stay the same, so ironman achievements stay available.
- Reading the game changes nothing. Keyboard, mouse and console commands do change your game, exactly as if you had pressed the keys yourself, and each of them needs its own switch.
- Navigation moves your camera and opens province views, as the game's own province links do; it is switched on together with keyboard and mouse.
- Loading a save by name is a console-level command: it discards the running game without saving it, as the game's own Load dialog does, and never saves for you.
- In ironman and multiplayer Anvil keeps Tongs asleep unless you allow it there yourself.
- Unsubscribe and it's gone. Nothing is left in your saves.

![](https://i.imgur.com/Uf8Z21n.png)

## More from Happy Dwarf Team
- **[Anvil](https://anvil.happydwarf.dev/)**: the plugin loader Tongs runs on. Without it, Tongs is just a folder; with it, you see which plugins load and switch each one on or off in Anvil Manager.
- **[Bellows](https://steamcommunity.com/sharedfiles/filedetails/?id=3823747170)**: test loops restart the game often; Bellows cuts each start from about 37 to about 13 seconds on our test machine.
- **[Etch](https://etch.happydwarf.dev/)**: for interface modders, see your `.gui` window as the game draws it, right in VS Code.
- Everything we make: [happydwarf.dev](https://happydwarf.dev/)

![](https://i.imgur.com/Uf8Z21n.png)

### Alternative download
[GitHub](https://github.com/happy-dwarf-team/tongs)

![](https://i.imgur.com/Uf8Z21n.png)

## FAQ

### Why do I have to approve it?
Tongs is a real program running inside your game, not just a pile of text files. Anvil makes sure nothing like that runs without your nod. Trust, but verify. Dwarf law.

### I subscribed and nothing happens.
That is on purpose: Tongs starts switched off. Turn it on in Anvil's panel or in Anvil Manager and restart the game. Then point your script at it; the GitHub page shows how.

### Can someone on the internet control my game?
No. Tongs only answers programs running on your own computer under your own Windows user. The front gate stays shut, and the dwarf keeps the key.

### Does it work with my mods?
It reads the interface the game is actually showing, so interface mods should not get in its way. The game itself has to be 1.37.5.

### What is that little dwarf head on the thumbnail?
If you check the mod's GitHub page, you will notice the repository belongs to Happy Dwarf Team.

I like dwarves.
I like being happy.
I am, in fact, a happy dwarf myself.

![](https://i.imgur.com/Uf8Z21n.png)

## Credits
**Anvil** for letting a dwarf into the forge.
**Every test agent that clicked the wrong button a thousand times** so yours doesn't have to.

![](https://anvil.happydwarf.dev/workshop/tongs-hero.png)

Tongs is for mod testers, tool makers and anyone who wants a program or an AI agent to watch a game of EU4 and press its buttons. On its own it does nothing you can see.

![](https://anvil.happydwarf.dev/workshop/tongs-install.png)

![](https://anvil.happydwarf.dev/workshop/tongs-steps.png)

Get Anvil Manager from [anvil.happydwarf.dev](https://anvil.happydwarf.dev/). Tongs needs Anvil 1.7 or newer (take the latest), EU4 1.37.5 (Steam; GOG's 1.37.5.2 and 1.37.5.0 too, see the FAQ) and something to talk to it: the [GitHub page](https://github.com/happy-dwarf-team/tongs) has clients for PowerShell and Python and an MCP server. If Windows warns about Anvil Manager, choose **More info**, then **Run anyway**.

![](https://anvil.happydwarf.dev/workshop/tongs-tiles-title.png)

![](https://anvil.happydwarf.dev/workshop/tongs-tiles.png)

Keyboard, mouse and console commands change your game exactly as if you had pressed the keys yourself. Loading a save by name discards the running game without saving it. In ironman and multiplayer Anvil keeps Tongs asleep unless you allow it there.

[![](https://anvil.happydwarf.dev/workshop/tongs-docs.png)](https://anvil.happydwarf.dev/plugins/tongs/) [![](https://anvil.happydwarf.dev/workshop/tongs-github.png)](https://github.com/happy-dwarf-team/tongs)

[![](https://anvil.happydwarf.dev/workshop/discord-en.png)](https://discord.happydwarf.dev/)

![](https://anvil.happydwarf.dev/workshop/tongs-faq.png)

### Why do I have to approve it?
Tongs is a real program running inside your game, not just a pile of text files. Anvil makes sure nothing like that runs without your nod. Trust, but verify. Dwarf law.

### I subscribed and nothing happens.
That is on purpose: Tongs starts switched off. Turn it on in Anvil's panel or in Anvil Manager and restart the game. Then point your script at it; the GitHub page shows how.

### Can someone on the internet control my game?
No. Tongs only answers programs running on your own computer under your own Windows user. The front gate stays shut, and the dwarf keeps the key.

### Can it play through events while I am away?
With input switched on, `run_until` runs the game to a date at the speed you ask and can choose the first option of each of your events on the way. It chooses them the way the event's button does, so it works with the window minimized too; in our tests eight events in a row went through, two of them minimized. Your script can also switch the map mode, a plugin's map modes included.

### Does it work with my mods?
It reads the interface the game is actually showing, so interface mods should not get in its way. The game itself has to be 1.37.5 (or GOG's 1.37.5.2 or 1.37.5.0).

### I play on GOG. Can I use it?
Yes, on the GOG builds 1.37.5.2 and 1.37.5.0, with the latest Anvil. GOG has no Workshop, so install Tongs from the **Catalog** tab in Anvil Manager, or from the release zip on [GitHub](https://github.com/happy-dwarf-team/tongs/releases). The tongs fit either hand.

### Something's wrong. Where do I ask?
In **#help-and-bugs** on our [Discord server](https://discord.happydwarf.dev/) (pick the Tongs tag), or in a [GitHub issue](https://github.com/happy-dwarf-team/tongs/issues/new/choose). What we confirm on Discord goes to GitHub too.

### What is that little dwarf head on the thumbnail?
If you check the mod's GitHub page, you will notice the repository belongs to Happy Dwarf Team.

I like dwarves.
I like being happy.
I am, in fact, a happy dwarf myself.

![](https://anvil.happydwarf.dev/workshop/tongs-credits.png)

**Anvil** for letting a dwarf into the forge.
**Every test agent that clicked the wrong button a thousand times** so yours doesn't have to.

[![](https://anvil.happydwarf.dev/workshop/collection-en.png)](https://steamcommunity.com/sharedfiles/filedetails/?id=3833899418)

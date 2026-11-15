# Two games on one computer, in one multiplayer game

With the `instance` setting, Tongs can drive two EU4 1.37.5 games on one computer that play one multiplayer game
together, for example to test that a mod or plugin stays in sync. This page says how to set the two games up so they
use the same playset and settings, on Linux and on Windows.

What is verified: on Linux (Proton), a GOG copy hosting and a Steam copy joining played one multiplayer game with the
full out-of-sync check on, each through its own Tongs instance, and stayed in sync for a game year (14 November 2026).
The Windows steps below follow the same rules but have not been tried on Windows yet.

## The rules that make it work

- **Two copies of the game**, one per player: a **GOG copy and a Steam copy**. Two Steam copies did not join each
  other ("Game not found"), probably because both use the same Steam account in the multiplayer service.
- **Nakama multiplayer**: start both with `-nakamamp`.
- **Two Paradox accounts**: each game is logged in to a different Paradox account. The GOG copy asks for the login the
  first time it opens the multiplayer menu (`pdx_online_login_dialog`); the login is kept in that game's profile.
- **Two player names**: each profile needs its own player name (`lastplayer` in `settings.txt`, or the name field in
  the multiplayer menu). With the same name, the join is refused with "Your name is taken".
- **The GOG copy hosts.** A game hosted by the Steam copy was never joinable in our tests.
- **The full out-of-sync check**: add `-hardoos` to the host's command line; the lobby then says "Hard OOS is enabled".
  The guest needs no flag, because the host's settings travel with the session.
- **One Tongs instance per game**: give each game its own `instance` (for example `host` and `guest`), so each serves
  its own pipe (`\\.\pipe\tongs-host`, `\\.\pipe\tongs-guest`) and, under Wine, its own bridge file. Point each client
  at its game with `--instance <name>` or `TONGS_INSTANCE=<name>`.

## The same playset and settings in both games

Give each game its own profile folder with `-userdir=<folder>` (the path must not contain spaces), and copy into it
from your normal profile (`Documents/Paradox Interactive/Europa Universalis IV`):

- `dlc_load.json` (the playset: which mods and DLC are on),
- `settings.txt` and `pdx_settings.txt` (the game's settings; then change `lastplayer` in one of them),
- the `mod/*.mod` descriptors of the mods in the playset. A descriptor with a relative `path="mod/<name>"` also needs
  that mod's folder in the new profile's `mod/`, or edit the descriptor to an absolute path.

Each game then reads and writes only its own profile, and your normal profile is left as it is. Windowed mode (for
example 1280×720) keeps both games on one screen.

Each copy of the game needs Anvil and Tongs installed and approved for that copy. In each copy's `anvil/anvil.toml`,
allow Tongs in multiplayer (`allow_multiplayer = ["tongs"]`), and give the copy its own instance, either in Tongs'
settings file (`instance = "host"`) or for one start with the environment variable `ANVIL_TONGS_INSTANCE=host`.

## Linux (Steam and GOG under Proton)

Verified. Both games run in the Steam prefix of EU4 (app 236850), started with `protontricks-launch`, so they share
one wineserver; that is why the instance names are needed.

1. Make the GOG copy runnable in the Steam prefix (a copy of the GOG installation with Anvil copied in from the Steam
   installation), and make the two profile folders as above.
2. Start the host: `ANVIL_TONGS_INSTANCE=host protontricks-launch --appid 236850 <gog-copy>/eu4.exe -nakamamp -hardoos -userdir=<host-profile>`.
3. Start the guest: `ANVIL_TONGS_INSTANCE=guest protontricks-launch --appid 236850 <steam-copy>/eu4.exe -nakamamp -userdir=<guest-profile>`.
4. Drive each with `tongs.py --instance host …` and `tongs.py --instance guest …` (or the MCP server with
   `TONGS_INSTANCE`).

Quit each game from its main menu. The lobby's **Back** button ended *both* games in our test, probably because the
game restarts itself and the shared wineserver goes down with the first launch.

## Windows

Not tried yet. Each game runs as its own Windows process, so the pipes would collide without `instance` too: the
second game cannot serve `\\.\pipe\tongs`.

1. Install the game from GOG and from Steam (two separate installation folders), and install Anvil and Tongs into each.
2. Make the two profile folders as above.
3. Start the host from the GOG folder: `set ANVIL_TONGS_INSTANCE=host` and then `eu4.exe -nakamamp -hardoos -userdir=C:\eu4\host`.
4. Start the guest from the Steam folder, through Steam so it is logged in there: set `ANVIL_TONGS_INSTANCE=guest`
   for that start (or `instance = "guest"` in that copy's Tongs settings), with the launch options
   `-nakamamp -userdir=C:\eu4\guest`.
5. Drive each with `tongs.py --instance host …` and `tongs.py --instance guest …`.

## Hosting and joining

Through Tongs, with the widget paths from our run:

1. **Host**: main menu, Multiplayer (`//multi_player_button`), `matchmaking_window//host`; type a name into
   `host_window/edit` and a password into `host_window/password`; leave `host_window/private_checkbox` off (a private
   game is not listed); `host_window/ok_host_button`. The lobby chat says "Hard OOS is enabled".
2. **Guest**: main menu, Multiplayer; type the name into `matchmaking_window/matchmaking_servers_window/filter_name`,
   refresh (`…/refresh_internet`), select the game, join, and type the password into `connect_window/password`
   (`connect_window/ok_join_button`).
3. **Start**: each player picks a nation in `country_selection_panel/lobby/right`; the guest clicks **Ready**, then the
   host clicks **PLAY**. Close the start screen in both (`startup_screen/close_button`).
4. **Run**: Tongs' `speed` and `resume` work on the host in multiplayer. Read `state` from both instances and watch
   `date`, `out_of_sync` and `paused_oos`.

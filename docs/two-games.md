# Two games on one computer, in one multiplayer game

With the `instance` setting, Tongs can drive two EU4 1.37.5 games on one computer that play one multiplayer game
together, for example to test that a mod or plugin stays in sync. This page says how to set the two games up so they
use the same playset and settings, on Linux and on Windows.

What is verified: on Linux (Proton), a GOG copy hosting and a Steam copy joining played one multiplayer game with the
full out-of-sync check on, each through its own Tongs instance, and stayed in sync for a game year (14 November 2026).
On Windows, a GOG copy hosting and a Steam copy joining did the same for 14 months of a new game (15 November 2026).

## The rules that make it work

- **Two copies of the game**, one per player. The simplest pair is a **GOG copy and a Steam copy**. Two Steam copies
  work too, but need extra steps after every start (see "Two Steam copies" below): on its own, every Steam copy logs in
  with the Paradox account linked to the Steam account, so two Steam copies are one player to the multiplayer service.
- **Nakama multiplayer**: start both with `-nakamamp`.
- **Two Paradox accounts**: each game is logged in to a different Paradox account. The GOG copy asks for the login the
  first time it opens the multiplayer menu (`pdx_online_login_dialog`); the login is kept in that game's profile, in
  `pdx_login_cache.txt`. That file holds a login token: treat it like a password, and never commit, upload or share it.
- **Two player names**: each profile needs its own player name (`lastplayer` in `settings.txt`, or the name field in
  the multiplayer menu). With the same name, the join is refused with "Your name is taken".
- **Who hosts.** With a GOG and a Steam copy, let the GOG copy host: in our first test a game hosted by the Steam copy
  was not joinable. With two Steam copies on two accounts, either one could host (15 November 2026).
- **The full out-of-sync check**: add `-hardoos` to the host's command line; the lobby then says "Hard OOS is enabled".
  The guest needs no flag, because the host's settings travel with the session.
- **One Tongs instance per game**: give each game its own `instance` (for example `host` and `guest`), so each serves
  its own pipe (`\\.\pipe\tongs-host`, `\\.\pipe\tongs-guest`) and, under Wine, its own bridge file. Point each client
  at its game with `--instance <name>` or `TONGS_INSTANCE=<name>`. Case does not count: Windows compares pipe names
  without it, so Tongs folds the name to lower case (`Host` and `host` are the same game, on Linux too).

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
game restarts itself and the shared wineserver goes down with the first launch. With each game in its own prefix (as in
"Two Steam copies" below), Back ends only its own game.

## Windows

Verified (15 November 2026, EU4 1.37.5, Anvil 1.23.0): a GOG 1.37.5.0 copy hosting with `-hardoos` and a Steam copy
joining played 14 months of a new game without an out-of-sync, each driven over its own pipe for about 40 minutes,
also across restarts of both games (Hamweard's Exit to Menu; the restarted game inherits `ANVIL_TONGS_INSTANCE` and
serves its pipe again). Each game runs as its own Windows process, so the pipes would collide without `instance`: the
second game cannot serve `\\.\pipe\tongs`, and waits for it with an error in the log.

1. Install the game from GOG and from Steam (two separate installation folders), and install Anvil and Tongs into each.
2. Make the two profile folders as above.
3. Start the host from the GOG folder: `set ANVIL_TONGS_INSTANCE=host` and then `eu4.exe -nakamamp -hardoos -userdir=C:\eu4\host`.
   At Multiplayer it opens the Paradox login; sign in with the second account. The login is kept in that profile.
4. Start the guest from the Steam folder, with Steam running: `set ANVIL_TONGS_INSTANCE=guest` and then
   `eu4.exe -nakamamp -userdir=C:\eu4\guest`. It logs in with the Steam-linked Paradox account. Steam's launch options
   cannot set an environment variable on Windows (the `NAME=value %command%` form works on Linux only), so for a copy
   started through Steam put `instance = "guest"` in that copy's `anvil/data/tongs/settings.toml` instead.
5. Drive each with `tongs.py --instance host …` and `tongs.py --instance guest …`.

Two copies on one Paradox account are not two players on Windows either: the second Steam copy found the hosted game,
but Join ended in "Connecting... Game not found." after about 90 s.

## Two Steam copies

Verified on Linux (15 November 2026, Tongs 0.34.0): two Steam copies joined in both directions and stayed in sync with
`-hardoos` on the host. Not tried on Windows yet (a GOG host and a Steam guest were, above).

Two things make them two players:

- **A second Paradox account in the second copy, switched by hand after every start.** A Steam copy logs in with the
  Steam-linked Paradox account each time it starts; copying another profile's login file does not change that. After
  the second copy has reached its main menu, log out of the Paradox account there and log in with the second account.
  The switch does not survive a restart, so it is needed after every start of that copy.
- **Each game in its own environment**, so that each keeps its own login and, under Wine, its own wineserver.

### On Linux

1. Make a second copy of the Steam installation (for example a reflink copy) and the two profile folders, as above.
2. Copy the Steam prefix of EU4 (`compatdata/236850`) to a folder of its own for the second copy.
3. Start the first copy as usual: `ANVIL_TONGS_INSTANCE=host protontricks-launch --appid 236850 <steam-copy-1>/eu4.exe -nakamamp -hardoos -userdir=<host-profile>`.
4. Start the second copy in its own prefix: `STEAM_COMPAT_DATA_PATH=<copied-prefix> ANVIL_TONGS_INSTANCE=guest protontricks-launch --appid 236850 <steam-copy-2>/eu4.exe -nakamamp -userdir=<guest-profile>`
   (protontricks uses `STEAM_COMPAT_DATA_PATH` when it is set).
5. In the second copy's main menu, switch to the second Paradox account, then host and join as below.

### On Windows

Not tried yet; these steps follow the same rules.

1. Make a second copy of the Steam installation folder (a plain folder copy), and install Anvil and Tongs into it.
2. Make the two profile folders as above. On Windows both games run as the same Windows user; the separate `-userdir`
   folders keep their profiles, and so their logins, apart.
3. Start the first copy through Steam, with the launch options `-nakamamp -hardoos -userdir=C:\eu4\host` and
   `instance = "host"` in that copy's Tongs settings (Steam's launch options set no environment variable on Windows).
4. Start the second copy from its own folder, with Steam running: `set ANVIL_TONGS_INSTANCE=guest` and then
   `eu4.exe -nakamamp -userdir=C:\eu4\guest`. Steam may refuse to start the same game twice through its own
   launcher, which is why this copy starts from its folder (not checked).
5. In the second copy's main menu, switch to the second Paradox account, then host and join as below.

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
   `date`, `out_of_sync` and `paused_oos`. With both games on one computer, the game may slow itself down (to speed 3
   in our Windows run) for the player who lags behind.

Two things we met on Windows that are the game's, not Tongs' (15 November 2026):

- The GOG host's lobby did not list the saves written by the Steam copy (the GOG copy had fewer DLC on), so we
  started a new game.
- A campaign continued from a loaded save left the multiplayer session on the host right after the start: the host
  ran on alone and the guest stopped. Not checked yet without any plugin.

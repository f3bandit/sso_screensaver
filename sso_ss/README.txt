SuperStation One / Retro Remake -- idle screensaver
=====================================================

FILES
  idle_watcher                 background daemon: watches real input
                                (filters analog stick drift/noise),
                                launches the screensaver when idle
  intro_screensaver             the fullscreen animation player
  intro_screensaver.sh          launcher (reads ssone_screensaver.conf
                                 to pick 4:3 or 16:9 frame data)
  intro_frames_4x3.bin           original aspect animation data
  intro_frames_169.bin           widescreen animation data
  install_ssone_screensaver.sh  settings menu + installer
  ssone_screensaver.conf        written by the installer (aspect +
                                 timeout); safe to delete, defaults
                                 to 4:3 / 3 minutes

INSTALL
  1. Copy this whole SSOne folder onto your MiSTer SD card, e.g. into
     /media/fat/linux/sso/ (or run the installer from anywhere --
     it copies files into place itself).
  2. Run install_ssone_screensaver.sh (Scripts menu, or SSH):
       bash install_ssone_screensaver.sh
     Uses a proper dialog/whiptail menu if available, otherwise falls
     back to plain numbered prompts.
  3. In the menu:
       - "Set Aspect Ratio" -- 4:3 (original) or 16:9 (widescreen)
       - "Set Idle Timeout" -- 1/2/5/10/15/30 minutes, or Never
       - "Install / Apply Settings" -- copies files, writes the
         config, hooks user-startup.sh, and cleanly stops/restarts
         anything already running
  4. Reboot (or start it immediately -- the installer prints the
     exact command to do that without rebooting).

RE-RUNNING THE INSTALLER
  Always safe. Change aspect ratio or timeout any time by running it
  again and picking "Install / Apply Settings" -- it always cleanly
  stops the currently-running watcher first, so nothing stacks up or
  overlaps.

BEHAVIOR
  60s startup delay after boot, then triggers after your configured
  idle timeout of genuine input inactivity (mouse movement, real key/
  button presses, or analog stick movement past a deadzone -- small
  drift/electrical noise does NOT count as activity, matching how
  MiSTer Super Attract Mode filters its own input). Only triggers
  while sitting at the menu (checked via /tmp/CORENAME == MENU, which
  also covers Console Mode) -- never interrupts a running game.

  On trigger: fullscreen animation, scaled to fit your screen. Any
  real input exits it immediately, returning to whatever was already
  showing (Console Mode or the stock menu) -- the screensaver only
  ever draws an overlay, it never switches cores, so there's nothing
  else to "return to."

MANUALLY CHECKING / STOPPING
  ps | grep idle_watcher
  kill -9 <pid>

UNINSTALL
  Run install_ssone_screensaver.sh and pick "Uninstall", or manually:
    ps | grep idle_watcher        (kill any pid shown)
    remove the added lines from /media/fat/linux/user-startup.sh
    rm -rf /media/fat/linux/sso

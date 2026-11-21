#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Launcher for the Windows port of Uncharted: The Nathan Drake Collection (Tkinter).

Chooses the game folder, which of the three games to start, the display and console options,
and runs run.py with them as environment variables. Launcher options live in
%APPDATA%/uncharted-launcher/settings.json.

Frozen with PyInstaller the same Uncharted.exe also runs the game without the window
(`--play`), run.py (`--run`) and the preparation scripts (`--script`), so a packaged port
needs no Python installation.
"""
import copy
import ctypes
import http.client
import json
import os
from pathlib import Path
import queue
import re
import runpy
import shutil
import stat
import struct
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request
import webbrowser
import zipfile
import zlib

FROZEN = getattr(sys, 'frozen', False)
PORT_DIR = Path(sys.executable).resolve().parent if FROZEN else Path(__file__).resolve().parent.parent
sys.path.insert(0, str(PORT_DIR / 'scripts'))
sys.path.insert(0, str(Path(__file__).resolve().parent))
import trophies  # noqa: E402  (scripts/trophies.py: the games' trophy lists)
import uncharted_i18n
import fsr4_opt  # noqa: E402  (the opt-in FSR 4 download)

DATA_DIR = Path(os.environ.get('BB_DATA_DIR', PORT_DIR))
# expanduser, not Path.home(): started without USERPROFILE or HOME, the launcher still opens.
CONFIG_DIR = Path(os.environ.get('APPDATA') or os.path.expanduser('~')) / 'uncharted-launcher'
CONFIG_FILE = CONFIG_DIR / 'settings.json'
# Portable (Game page): the launcher's settings live in this file next to the launcher instead of %APPDATA%;
# the file being there is the switch.
PORTABLE_FILE = DATA_DIR / 'launcher_settings.json'
if PORTABLE_FILE.is_file():
    CONFIG_DIR, CONFIG_FILE = DATA_DIR, PORTABLE_FILE
NAME = 'Uncharted'
AUTHOR = 'SuperMedo · fork by Mevlock'  # shown after 'by '
# This build; GitHub release tags are v<VERSION> (their digits are compared).
VERSION = '0.3.6.66'  # fork: upstream 0.3.6 + this fork's changes
# Fork: updates come from this fork's own releases (the latest one; -pre tags are published as normal releases).
RELEASES_API = 'https://api.github.com/repos/Mevlock/uncharted_pc-dev/releases/latest'
RELEASES_PAGE = 'https://github.com/Mevlock/uncharted_pc-dev/releases/latest'
# Every release (Advanced > Version): the same repository's list, newest 100.
RELEASES_LIST_API = 'https://api.github.com/repos/Mevlock/uncharted_pc-dev/releases?per_page=100'
UPDATES = True
UPDATE_DIR = Path(tempfile.gettempdir()) / 'uncharted-update'
# What went wrong after a version switch that worked (the switcher has no window): outside UPDATE_DIR, which the
# launcher's start clears.
SWITCH_LOG = Path(tempfile.gettempdir()) / 'uncharted-switch.log'
# Never copied over an installation by an update (the package does not hold them either).
USER_FILES = ('user', 'out', 'art', 'bbport.ini', 'input.ini', 'last_run.log', 'launcher_settings.json')
# Advanced > Version: other releases of the port, none older than OLDEST_SWITCH_VERSION. The release left is kept
# in versions/<version> next to the launcher (only the most recent one, about 390 MB), for a switch back without a
# download; Remove backups deletes versions/.
OLDEST_SWITCH_VERSION = (0, 3, 6, 57)
VERSIONS_DIR = PORT_DIR / 'versions'
# versions/.incoming: a downloaded release copied to the installation's drive before a switch (%TEMP% on another).
INCOMING_DIR = '.incoming'
# What a release needs to start (every package has them): a kept backup or a source without one of them is not whole.
WHOLE_RELEASE = ('Uncharted.exe', '_internal', 'bin')
# The exact release of a package (its tag, as v0.3.6.65.1-test), written by a release build (package.sh): VERSION has
# no tag suffix, so a test build's backup would otherwise pass for the release with the same number.
RELEASE_FILE = 'release.txt'
KEEP_BACKUPS = 1
# The first release with Advanced > Version: an older one comes back to the newest through its Update button.
FIRST_SWITCH_VERSION = (0, 3, 6, 65)
# The top-level entries of the Windows package (packaging/windows/package.sh): what a version switch moves into
# the backup, together with whatever else the incoming release ships.
PROGRAM_FILES = ('Uncharted.exe', '_internal', 'Play Uncharted.exe', 'bin', 'launcher', 'scripts', 'shaders',
                 'notes', 'licenses', 'run.py', 'LICENSE', 'README.txt', 'commit.txt', RELEASE_FILE)
# Never moved or replaced by a version switch: the player's files, the backups, the opt-in FSR 4 download.
SWITCH_KEEP = USER_FILES + ('versions', 'fsr4_shaders')
# The folders a player chooses (the saves folder with the shader cache, the game folders): never inside what a
# version switch moves or Remove backups deletes (folders_inside).
PLAYER_FOLDERS = ('user_dir', 'game_dir', 'game_dir_u1', 'game_dir_u2', 'game_dir_u3')
MAX_LOG_LINES = 6000
NO_WINDOW = getattr(subprocess, 'CREATE_NO_WINDOW', 0)

# Naughty Dog's debug-menu switches the runtime keeps set (scripts/cheats.py KEEP; after BilalReloaded): the games
# that have them.
SWITCH_CHEATS = {'u1': ('noreload', 'onehit', 'grenades'), 'u2': ('noreload', 'onehit', 'grenades'), 'u3': ('noreload', 'onehit', 'grenades')}
SWITCH_CHEAT_TITLES = {'noreload': 'No Reload', 'onehit': 'One-Hit Kills', 'grenades': 'Infinite Grenades'}
GAMES = [
    ('u1', "Drake's Fortune", 'Uncharted 1 · 2007', 'eboot.bin'),
    ('u2', 'Among Thieves', 'Uncharted 2 · 2009', 'big2-ps4_Shipping.elf'),
    ('u3', "Drake's Deception", 'Uncharted 3 · 2011', 'big3-ps4_Shipping.elf'),
]
PORTABLE_HINT = "The launcher's own settings are kept in launcher_settings.json next to the launcher instead of your Windows user profile (%APPDATA%), so the folder can be moved or copied to another PC with them. Saves and shaders are kept here already, unless you chose another Saves folder above."
# Console language (system parameter 1), as the PS4 numbers them.
LANGUAGES = [('1', 'English (United States)'), ('18', 'English (United Kingdom)'), ('2', 'French'),
             ('22', 'French (Canada)'), ('4', 'German'), ('5', 'Italian'), ('3', 'Spanish'),
             ('20', 'Spanish (Latin America)'), ('7', 'Portuguese'), ('17', 'Portuguese (Brazil)'),
             ('6', 'Dutch'), ('8', 'Russian'), ('16', 'Polish'), ('19', 'Turkish'), ('21', 'Arabic'),
             ('0', 'Japanese'), ('9', 'Korean'), ('11', 'Chinese (Simplified)'), ('10', 'Chinese (Traditional)'),
             ('12', 'Finnish'), ('13', 'Swedish'), ('14', 'Danish'), ('15', 'Norwegian')]
PRESENT_MODES = [('Mailbox', 'Mailbox (low latency, no tearing)'), ('Fifo', 'FIFO (VSync)'),
                 ('FifoRelaxed', 'FIFO Relaxed'), ('Immediate', 'Immediate (tearing)')]
# The display rate the games run at (BB_VBLANK_HZ): they time gameplay with the real clock, and
# the prerecorded cutscenes keep 60 frames per second at every setting.
FRAME_RATES = [('60', '60 FPS (as on PS4)'), ('120', '120 FPS'), ('144', '144 FPS (experimental)'),
               ('165', '165 FPS (experimental)'), ('240', '240 FPS (experimental)')]
# A cap on the frames shown per second (BB_FPS_LIMIT), apart from the rate the games run at:
# 30 or 45 for a slower PC or a laptop on battery, 90 for a 90 Hz screen.
# The game's idle job threads (BB_JOB_SLEEP): spinning keeps the camera smooth; sleeping saves CPU.
# Even presents (vk_low_latency.cpp): each frame shown at an even moment, the world stepped by the same
# steps. Off (the default setting): BB_EVEN_PRESENT=0, each frame shown as soon as it is ready; Balanced:
# BB_EVEN_PRESENT=1 with the game's short GPU holds (BB_EVEN_RELEASE=4 when unset); Smoothest: also the full GPU
# hold (BB_EVEN_RELEASE=1). Every choice sets BB_EVEN_PRESENT; the Environment field still wins over it.
FRAME_PACING = [('balanced', 'Balanced'), ('smoothest', 'Smoothest (uses more GPU)'), ('off', 'Off')]
# Even presents are off in the game unless BB_EVEN_PRESENT=1 (opt-in); Smoothest adds the full GPU hold.
FRAME_PACING_ENV = {'balanced': (('BB_EVEN_PRESENT', '1'),),
                    'smoothest': (('BB_EVEN_PRESENT', '1'), ('BB_EVEN_RELEASE', '1')),
                    'off': (('BB_EVEN_PRESENT', '0'),)}
JOB_THREADS = [('auto', 'Automatic (spin on CPUs with more than 8 threads)'), ('spin', 'Spin (smoothest camera)'),
               ('sleep', 'Sleep (saves CPU)')]
FPS_LIMITS = [('0', 'Off (the frame rate above)'), ('30', '30 FPS'), ('45', '45 FPS'),
              ('60', '60 FPS'), ('90', '90 FPS'), ('120', '120 FPS'), ('144', '144 FPS')]
# The games render 1920x1080; the window (or the screen in fullscreen) gets that image scaled
# to its size: FSR 1 (EASU) or bilinear, then RCAS sharpening (BB_RCAS_ATTENUATION, lower
# is sharper).
WINDOW_SIZES = [('1280x720', '1280 × 720'), ('1600x900', '1600 × 900'),
                ('1920x1080', "1920 × 1080 (the games' own)"), ('2560x1440', '2560 × 1440'),
                ('1680x1050', '1680 × 1050 (16:10)'), ('1920x1200', '1920 × 1200 (16:10)'), ('2560x1600', '2560 × 1600 (16:10)'),
                ('3840x2160', '3840 × 2160 (4K)'), ('2560x1080', '2560 × 1080 (21:9)'),
                ('3440x1440', '3440 × 1440 (21:9)'), ('3840x1600', '3840 × 1600 (21:9)'),
                ('5120x1440', '5120 × 1440 (32:9)')]
# The size the games render at (BB_RENDER_RES): the game's display
# mode, memory budget, camera and movies are patched in memory. Its GPU memory stays under 4 GiB,
# which allows about 3440 x 1440 (RENDER_PIXELS_MAX); above that the largest render target heap
# moves to the CPU area (Uncharted 1: its tail) and the games go up to 3840 x 2160
# (RENDER_PIXELS_MAX_BIG; runtime_ultrawide.c). A larger size is scaled down to the limit.
# Native renders at the output size (the window, or the screen in fullscreen); the scaled modes
# render below it and the anti-aliasing (DLSS, FSR) upscales the frame to it before the HUD
# (BB_OUTPUT_RES), like DLSS's own modes.
RENDER_RESOLUTIONS = [('1080', "1920 × 1080 (the game's own)"),
                      ('auto', 'Automatic (native; 1920 × 1080 on cards with 6 GB or less)'),
                      ('native', 'Native (the window, or the screen in fullscreen)'),
                      ('quality', 'Quality: upscaled from 67%'),
                      ('balanced', 'Balanced: upscaled from 58%'),
                      ('performance', 'Performance: upscaled from 50%'),
                      ('3840x2160', '3840 × 2160 (4K, experimental)'),
                      ('3840x1600', '3840 × 1600 (21:9 4K, experimental)'),
                      ('4k-quality', '4K output, Quality: from 67% (experimental)'),
                      ('4k-balanced', '4K output, Balanced: from 58% (experimental)'),
                      ('4k-performance', '4K output, Performance: from 50% (experimental)'),
                      ('ss-quality', 'Supersampled 2.25x: from native (experimental)'),
                      ('ss-performance', 'Supersampled 2.25x, faster: from 75% (experimental)')]
# The 4K choices draw at that size whatever the window (scaled to it when presented); only they
# go above RENDER_PIXELS_MAX, which Native and the scaled modes keep.
RENDER_4K = {'3840x2160', '3840x1600'}
# The 4K output choices: the scaled modes with a 4K output instead of the window's size (3840 x 1600 on
# screens wider than 2:1, else 3840 x 2160), which the presenter then scales to the window: the upscaler's
# 4K picture supersampled to the screen, like NVIDIA's DLDSR. The render size stays that fraction of 4K.
RENDER_4K_UPSCALED = {'4k-quality': 1.5, '4k-balanced': 1.7, '4k-performance': 2.0}
# The supersampled choices (like NVIDIA's DLDSR 2.25x): rendered at the output size (or 75% of it), upscaled by the
# anti-aliasing to 1.5 times it per axis and filtered back down by the presenter (post_process.frag, Lanczos).
# The upscaled size stays within SUPERSAMPLED_PIXELS_MAX (a 3440 x 1440 screen's 1.5x); larger screens get less.
SUPERSAMPLED = {'ss-quality': 1.0, 'ss-performance': 0.75}
SUPERSAMPLED_PIXELS_MAX = 5160 * 2160
RENDER_SCALES = {'native': 1.0, 'quality': 1.5, 'balanced': 1.7, 'performance': 2.0}
RENDER_PIXELS_MAX = 3440 * 1440
RENDER_PIXELS_MAX_BIG = 3840 * 2160  # the 4K choices
# Wider than 16:9 (BB_ULTRAWIDE): the games' view is widened to the window, or the screen in
# fullscreen, and the picture fills it; in memory, the game files are not changed.
ULTRAWIDE = [('auto', 'Fill wide screens (wider view)'),
             ('always', 'Always fill (menus stretched)'),
             ('0', "16:9 with black bars (the games' own)")]
# The games' field of view in percent (BB_FOV), on every screen.
FIELDS_OF_VIEW = [('100', "100% (the games' own)"), ('110', '110%'), ('120', '120%'), ('130', '130%')]
SCALERS = [('fsr1', 'FSR 1 (AMD FidelityFX, sharp)'), ('bilinear', 'Smooth (bilinear)')]
# The games' depth of field (BB_DOF): Uncharted 2 and 3 blur the background while aiming and in some scenes.
DEPTH_OF_FIELD = [('1', "On (the games' own)"), ('aim', 'Off only while aiming'), ('0', 'Off everywhere')]
# Anisotropic filtering of scene textures (BB_ANISO; 0 = the game's own).
ANISOTROPY = [('0', "Off (the game's own)"), ('4', '4x'), ('8', '8x'), ('16', '16x')]
SHARPENING = [('off', 'Off'), ('1000', 'Low'), ('500', 'Medium'), ('250', 'High'), ('0', 'Maximum')]
# HDR10 output (BB_HDR, BB_HDR_PEAK_NITS, BB_HDR_PAPER_WHITE; off by default).
HDR_MODES = [('off', 'Off'), ('on', 'On')]
# Scene HDR tone mapping (BB_HDR_SCENE_MODE; the in-game menu's Tone mapping starts from this choice).
HDR_TONE_MODES = [('2', 'SDR + highlights'), ('1', 'Game curve extended'), ('0', 'RenoDX tone mapping')]
HDR_PEAK_NITS = [('auto', 'Auto (as Windows reports)'), ('400', '400'), ('600', '600'), ('800', '800'), ('1000', '1000'), ('1500', '1500'), ('2000', '2000'), ('4000', '4000'), ('10000', '10000')]
HDR_PAPER_WHITE = [('80', '80'), ('100', '100'), ('150', '150'), ('203', '203'), ('250', '250'), ('300', '300'), ('400', '400'), ('500', '500')]
# Temporal anti-aliasing at the render resolution (BB_UPSCALER): it takes the scene before the
# games' post-processing with their own motion vectors, so it replaces their edge smoothing. At a
# reduced render resolution (Uncharted 3: Quality, Balanced, Performance) it also upscales.
ANTI_ALIASING = [('off', "Off (the game's own)"), ('taa', 'TAA'), ('fsr3', 'AMD FSR 3.1 (Native AA)'),
                 ('fsr4', 'AMD FSR 4 (Native AA)'), ('dlss', 'NVIDIA DLSS / DLAA (GeForce RTX)')]
FSR411 = ('fsr411', 'AMD FSR 4.1.1 (Native AA)')
XESS = ('xess', 'Intel XeSS (Native AA, any GPU)')
# The DLSS model (BB_DLSS_PRESET): the driver's default for DLAA is preset K (DLSS 4);
# L and M are the DLSS 4.5 models.
DLSS_MODELS = [('default', "Driver default (K, DLSS 4)"), ('K', 'K (DLSS 4)'),
               ('L', 'L (DLSS 4.5)'), ('M', 'M (DLSS 4.5)')]
AA_SHARPNESS = [('0', 'Off'), ('0.2', 'Low'), ('0.4', 'Medium'), ('0.7', 'High'), ('1.0', 'Very high'),
                ('1.5', 'Maximum')]
# Frames generated between two of the game's (BB_FRAME_GEN, BB_FRAME_GEN_MULTIPLIER).
FRAME_GENERATION = [('off', 'Off'), ('fsr3', 'AMD FSR 3 Frame Generation (any GPU)'),
                    ('dlss', 'NVIDIA DLSS Frame Generation (GeForce RTX 40/50)')]
LOW_LATENCY = [('0', 'Off'), ('1', 'On'), ('2', 'On + boost (NVIDIA: GPU clocks stay up)')]
# The speaker layout the games are told (BB_SPEAKERS; from drakolordx7's upstream change, PR #392).
SPEAKERS = [('auto', 'Automatic (the Windows speaker setup)'), ('2', 'Stereo'), ('6', '5.1 surround'),
            ('8', '7.1 surround')]
XBOX_STYLES = [('antik', 'antikmewzik (hand-drawn, coloured halos)'), ('bilal', 'BilalReloaded (dark buttons, coloured letters)')]
BUTTON_PROMPTS = [('auto', 'Automatic (follows the device you used last)'),
                  ('keyboard', 'Keyboard and mouse keys'), ('xbox', 'Xbox buttons'), ('nintendo', 'Nintendo buttons (B A Y X)'),
                  ('0', "PlayStation buttons (the game's own)")]
FRAME_GEN_MULTIPLIERS = [('2', '2× (one generated frame)'), ('3', '3× (DLSS on GeForce RTX 50)'),
                         ('4', '4× (DLSS on GeForce RTX 50)')]
# Controls (input.ini, read by the game: src/runtime_pad.c). Actions with their PS4 names, the
# default keys ("A|B": two bindings) and the default controller buttons (SDL names).
BUTTON_ACTIONS = [
    ('cross', 'Cross ✕', 'Space', 'a'), ('circle', 'Circle ○', 'C', 'b'),
    ('square', 'Square □', 'F', 'x'), ('triangle', 'Triangle △', 'E', 'y'),
    ('l1', 'L1', 'Q', 'leftshoulder'), ('r1', 'R1', 'G', 'rightshoulder'),
    ('l2', 'L2 (aim)', 'Mouse Right', 'lefttrigger'), ('r2', 'R2 (fire)', 'Mouse Left', 'righttrigger'),
    ('l3', 'L3', 'Left Shift', 'leftstick'), ('r3', 'R3', 'V', 'rightstick'),
    ('options', 'Options', 'Escape|Return', 'start'), ('touchpad', 'Touchpad', 'Tab', 'back'),
    ('up', 'D-pad up', 'Up', 'dpup'), ('down', 'D-pad down', 'Down', 'dpdown'),
    ('left', 'D-pad left', 'Left|Wheel Up', 'dpleft'), ('right', 'D-pad right', 'Right|Wheel Down', 'dpright'),
]
STICK_ACTIONS = [
    ('ls_up', 'Move forward', 'W'), ('ls_down', 'Move back', 'S'),
    ('ls_left', 'Move left', 'A'), ('ls_right', 'Move right', 'D'),
    ('rs_up', 'Look up', 'I'), ('rs_down', 'Look down', 'K'),
    ('rs_left', 'Look left', 'J'), ('rs_right', 'Look right', 'L'),
    ('rotate_ccw', 'Turn valve / wheel counter-clockwise (hold)', 'R'),
    ('rotate_cw', 'Turn valve / wheel clockwise (hold)', 'T'),
    ('walk', 'Walk (hold with the movement keys)', 'Left Ctrl'),
]
# Hold a rotate key and the left stick circles: the games' valves, wheels and cranks ask for the stick
# turned in circles, which the eight movement-key directions cannot do. Walk: the movement keys push the
# stick part way (walk_speed). Both by drakolordx7, from his fork (46aec0d3, 6455d6bb).
# Keys added after players already had their own bindings: the default is left out where the player
# already uses that key for something else.
LATE_ACTIONS = ('rotate_ccw', 'rotate_cw', 'walk')
WALK_SPEED_MIN, WALK_SPEED_MAX = 50, 100
DEFAULT_KEYS = {a: k for a, _t, k, _p in BUTTON_ACTIONS} | {a: k for a, _t, k in STICK_ACTIONS}
DEFAULT_PAD = {a: p for a, _t, _k, p in BUTTON_ACTIONS}
PAD_BUTTONS = [('a', 'A / Cross'), ('b', 'B / Circle'), ('x', 'X / Square'), ('y', 'Y / Triangle'),
               ('leftshoulder', 'LB / L1'), ('rightshoulder', 'RB / R1'),
               ('lefttrigger', 'LT / L2'), ('righttrigger', 'RT / R2'),
               ('leftstick', 'Left stick click / L3'), ('rightstick', 'Right stick click / R3'),
               ('start', 'Start / Options'), ('back', 'Back / Share'), ('touchpad', 'Touchpad click'),
               ('dpup', 'D-pad up'), ('dpdown', 'D-pad down'), ('dpleft', 'D-pad left'),
               ('dpright', 'D-pad right'), ('misc1', 'Share / Mic (misc)'), ('paddle1', 'Paddle 1'),
               ('paddle2', 'Paddle 2'), ('paddle3', 'Paddle 3'), ('paddle4', 'Paddle 4'),
               ('none', 'Not assigned')]
MOUSE_SENSITIVITY = [('0.25', '0.25'), ('0.5', '0.5'), ('0.75', '0.75'), ('1.0', '1.0 (default)'),
                     ('1.25', '1.25'), ('1.5', '1.5'), ('2.0', '2.0'), ('2.5', '2.5'), ('3.0', '3.0'),
                     ('4.0', '4.0')]
# Tk key names → SDL key names (letters and digits map to themselves, upper case).
TK_KEYS = {'space': 'Space', 'Return': 'Return', 'Escape': 'Escape', 'Tab': 'Tab', 'BackSpace': 'Backspace',
           'Shift_L': 'Left Shift', 'Shift_R': 'Right Shift', 'Control_L': 'Left Ctrl',
           'Control_R': 'Right Ctrl', 'Alt_R': 'Right Alt', 'Caps_Lock': 'CapsLock', 'Up': 'Up',
           'Down': 'Down', 'Left': 'Left', 'Right': 'Right', 'Delete': 'Delete', 'Home': 'Home',
           'End': 'End', 'Prior': 'PageUp', 'Next': 'PageDown', 'minus': '-', 'equal': '=',
           'Menu': 'Application', '[': '[', ']': ']', 'bracketleft': '[', 'bracketright': ']',
           'semicolon': ';', 'apostrophe': "'", 'comma': ',', 'period': '.', 'slash': '/', 'backslash': '\\', 'grave': '`', 'KP_Enter': 'Keypad Enter',
           'KP_Add': 'Keypad +', 'KP_Subtract': 'Keypad -', 'KP_Multiply': 'Keypad *',
           'KP_Divide': 'Keypad /', 'KP_Decimal': 'Keypad .', 'KP_Delete': 'Keypad .',
           'KP_Insert': 'Keypad 0', 'KP_End': 'Keypad 1', 'KP_Down': 'Keypad 2', 'KP_Next': 'Keypad 3',
           'KP_Left': 'Keypad 4', 'KP_Begin': 'Keypad 5', 'KP_Right': 'Keypad 6', 'KP_Home': 'Keypad 7',
           'KP_Up': 'Keypad 8', 'KP_Prior': 'Keypad 9'}
# Tk on Windows names numpad keys like the main ones ("8", "plus"), only the keycode tells them apart (#224).
WIN_NUMPAD = {**{0x60 + n: f'Keypad {n}' for n in range(10)},
              0x6A: 'Keypad *', 0x6B: 'Keypad +', 0x6D: 'Keypad -', 0x6E: 'Keypad .', 0x6F: 'Keypad /'}
MOUSE_SENS_MIN, MOUSE_SENS_MAX = 0.1, 10.0  # the Controls page slider (default 2.8: the old mouse look's feel; 1 = Bilal's pace)
# Times the sensitivity while aiming, and for up and down (1 = the same as looking / sideways).
MOUSE_RATIO_MIN, MOUSE_RATIO_MAX = 0.25, 4.0
CAMERA_VERTICAL_MAX = 8.0   # the normal camera's up / down (its arc is 1.3-5x slower than sideways)
# Insert opens the in-game settings menu; Left Alt frees the mouse cursor.
RESERVED_KEYS = {'Insert': 'Insert opens the in-game settings menu.',
                 'Alt_L': 'Left Alt frees the mouse cursor while held.'}

APP_DEFAULTS = {'game_dir': '', 'game_dir_u1': '', 'game_dir_u2': '', 'game_dir_u3': '', 'user_dir': '', 'portable': False, 'start': 'u1', 'language': '1', 'ui_language': '', 'player_name': '',
                'fullscreen': False, 'pause_in_background': False, 'present_mode': 'Mailbox', 'frame_rate': '60', 'fps_limit': '0', 'frame_pacing': 'off', 'show_fps': False, 'hdr': 'off', 'hdr_tone': '2', 'hdr_peak_nits': '1000', 'hdr_paper_white': '203', 'bloom': True, 'depth_of_field': '1', 'skip_intro': False, 'job_threads': 'auto', 'unlock_chapters': False, 'unlock_rewards': False, 'prep_unknown_versions': False, 'skip_friends_notice': True, 'anisotropy': '0', 'full_res_textures': False, 'hires_shadows': False,
                'window_size': '1920x1080', 'render_resolution': '1080', 'ultrawide': 'auto', 'fov': '100', 'scaler': 'fsr1', 'sharpening': '500',
                'anti_aliasing': 'off', 'dlss_model': 'default', 'aa_sharpness': '0.2', 'frame_generation': 'off', 'low_latency': '0',
                'frame_gen_multiplier': '2', 'speakers': 'auto', 'extra_env': '', 'close_on_play': False,
                'shader_preparation': 'before', 'preparation_speed': 'all',
                'mouse_look': True, 'mouse_sensitivity': '2.8', 'mouse_invert_y': False,
                'mouse_aim_sensitivity': '0.6', 'mouse_vertical': '1.0', 'mouse_camera_vertical': '2.0',
                'pad_invert_y': False, 'pad_background': False, 'pad_vibration': True, 'pad_vibration_strength': '100', 'walk_speed': '65', 'button_prompts': 'auto', 'xbox_style': 'antik', 'key_bindings': {}, 'pad_bindings': {}, 'check_updates': True, 'show_test_builds': False, 'notes_hide_after_update': False,
                'trophy_game': 'u1', 'art_banner': '', 'art_u1': '', 'art_u2': '', 'art_u3': '', 'art_crops': {},
                **{f'{game}_infinite_{cheat}': False for game, _t, _s, _e in GAMES for cheat in ('health', 'ammo')},
                **{f'{game}_devmode': False for game, _t, _s, _e in GAMES},
                **{f'{game}_devmode_text': True for game, _t, _s, _e in GAMES},
                **{f'{game}_{cheat}': False for game, cheats in SWITCH_CHEATS.items() for cheat in cheats}}


# Advanced > Reset all settings keeps where things are and who plays: the game and saves folders, the
# game chosen, the console and launcher languages, the player name, the cover art and the trophy page's
# game. Everything else in APP_DEFAULTS (display, controls, cheats, game options, advanced) goes back.
RESET_KEEP = {'game_dir', 'game_dir_u1', 'game_dir_u2', 'game_dir_u3', 'user_dir', 'start', 'language',
              'ui_language', 'player_name', 'trophy_game', 'art_banner', 'art_u1', 'art_u2', 'art_u3',
              'art_crops', 'notes_hide_after_update'}


def reset_settings(s):
    """The settings s with every option outside RESET_KEEP back to its default (keys the launcher keeps
    for itself, outside APP_DEFAULTS, are left alone)."""
    return {**s, **{key: copy.deepcopy(value) for key, value in APP_DEFAULTS.items() if key not in RESET_KEEP}}


UI_LANGUAGE = 'en'


def _(text):
    """Translate launcher text, keeping English as the fallback."""
    return uncharted_i18n.translate(UI_LANGUAGE, text)


# ---------------------------------------------------------------------------------------------
# Command line roles of the frozen executable.

def attach_stdio():
    """A windowed executable starts without sys.stdout; inherited pipes or a console still exist."""
    import msvcrt
    for name, std in (('stdout', -11), ('stderr', -12)):
        if getattr(sys, name) is not None:
            continue
        stream = None
        handle = ctypes.windll.kernel32.GetStdHandle(std)
        if handle and handle != ctypes.c_void_p(-1).value:
            try:
                stream = open(msvcrt.open_osfhandle(handle, os.O_WRONLY), 'w', encoding='utf-8',
                              errors='replace', buffering=1)
            except OSError:
                stream = None
        setattr(sys, name, stream or open(os.devnull, 'w'))


def run_role(argv):
    """--run [args]: run.py; --script <file> [args]: a preparation script. Returns an exit code."""
    attach_stdio()
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(line_buffering=True, errors='replace', **({} if stream.isatty() else {'encoding': 'utf-8'}))
        except (AttributeError, ValueError):
            pass
    if argv[0] == '--script':
        path, sys.argv = argv[1], argv[1:]
    else:
        path = str(PORT_DIR / 'run.py')
        sys.argv = [path, *argv[1:]]
    try:
        runpy.run_path(path, run_name='__main__')
    except SystemExit as stop:
        return stop.code if isinstance(stop.code, int) else (0 if stop.code is None else 1)
    return 0


def run_command():
    return [sys.executable, '--run'] if FROZEN else [sys.executable, str(PORT_DIR / 'run.py')]


# ---------------------------------------------------------------------------------------------
# Settings.

def load_json(path, default):
    try:
        return json.loads(Path(path).read_text(encoding='utf-8-sig'))
    except (OSError, ValueError):
        return default


def ini_path():
    return Path(os.environ.get('BB_CONFIG', DATA_DIR / 'bbport.ini'))


def write_atomic(path, text):
    """Writes a whole file through a temporary file and a rename, so a crash or power loss mid-write leaves
    the old file, never an empty or half-written one (poncho-ajmv's #605)."""
    path = Path(path)
    tmp = path.with_name(path.name + '.tmp')
    with open(tmp, 'w', encoding='utf-8') as f:  # same newlines as the write_text it replaces
        f.write(text)
        f.flush()
        os.fsync(f.fileno())
    for attempt in range(5):  # another program reading the file can briefly block the rename on Windows
        try:
            os.replace(tmp, path)
            return
        except PermissionError:
            if attempt == 4:
                raise
            time.sleep(0.05)


def set_ini(key, value):
    """bbport.ini (read by the game; the in-game menu edits it): one key rewritten in place."""
    path = ini_path()
    try:
        lines = path.read_text(encoding='utf-8').splitlines()
    except OSError:
        lines = []
    out, found = [], False
    for line in lines:
        if '=' in line and not line.lstrip().startswith('#') and line.split('=', 1)[0].strip() == key:
            out.append(f'{key}={value}')
            found = True
        else:
            out.append(line)
    if not found:
        out.append(f'{key}={value}')
    write_atomic(path, '\n'.join(out) + '\n')


def game_info(folder):
    """{'title', 'id', 'version', 'games': [keys present]} of a game folder, or None."""
    folder = Path(folder or '.')
    if not any((folder / exe).is_file() for _k, _t, _s, exe in GAMES):
        return None
    info = {'title': 'Uncharted', 'id': folder.name, 'version': '?',
            'games': [key for key, _n, _s, exe in GAMES if (folder / exe).is_file()]}
    try:
        from prepare import sfo
        values = sfo((folder / 'sce_sys/param.sfo').read_bytes())
        # The newer of APP_VER and VERSION: a package with an update built in (the Spanish EP9000-CUSA02826
        # A0100-V0102, EU/Russian CUSA02344 1.02 dumps) keeps APP_VER 01.00 and says 01.02 in VERSION.
        versions = [v for v in (values.get('APP_VER'), values.get('VERSION')) if v]
        info.update(title=values.get('TITLE', info['title']).replace('™', '').strip(),
                    id=values.get('TITLE_ID', info['id']),
                    version=max(versions, key=lambda v: tuple(int(p) for p in v.split('.') if p.isdigit()))
                    if versions else '?')
    except (OSError, ValueError, ImportError):
        pass
    return info


def shader_list(key):
    """The variant list shipped for one game (shaders/<game>_variants.txt) and its pipeline keys, or None."""
    ship = PORT_DIR / 'shaders' / f'{key}_variants.txt'
    return (ship, ship.with_name(f'{key}_keys.txt')) if ship.is_file() else None


CLEARED_CACHE = 'cache.cleared-'  # user/cache renamed by Clear shader cache, waiting to be deleted


def delete_cleared_caches():
    """Deletes the caches set aside by Clear shader cache (also ones a closed launcher left half deleted)."""
    for folder in (DATA_DIR / 'user').glob(f'{CLEARED_CACHE}*'):
        shutil.rmtree(folder, ignore_errors=True)


def shader_cache_dirs(s, key):
    """The game's shader cache folders (<title id>-<build>, not the dotted backups)."""
    info = game_info(game_folder(s, key))
    root = DATA_DIR / 'user' / 'cache'
    if not info or not root.is_dir():
        return []
    return [p for p in root.glob(f"{info['id']}*") if p.is_dir() and '.' not in p.name]


def shaders_prepared(s, key):
    """True when the game left a marker for the current list (its size in it), False when not, None with no list.
    The marker's line may go on after the size: AMD and Intel graphics write '<size> clip-jitter <version>' (the
    upscaler jitter in the vertex shaders changes their pipelines). Comparing the whole line, the launcher said 'not
    prepared yet' on those cards after every preparation while the game itself found the marker."""
    lists = shader_list(key)
    if not lists:
        return None
    stamp = str(lists[0].stat().st_size)
    for folder in shader_cache_dirs(s, key):
        try:
            words = (folder / f'prepared_{lists[0].stem}').read_text().split('\n')[0].split()
            if words and words[0] == stamp:
                return True
        except OSError:
            pass
    return False




# The two builds of the game code, by SHA-1 of the segments an executable loads (eboot, Uncharted 2, Uncharted 3; not
# its signature header or the metadata a dumper adds at the end), or of the whole file for an executable that is not
# signed. 1.00: the original build, in every region (US 1.00; EU dumps whose files were never updated, even when
# their param.sfo says 01.02). 1.02: the updated build (US 1.02 CUSA02320, EU/Russian CUSA02344).
GAME_CODE = {
    'cd749c2f217a842200a9867f7c8af0973c990a74': '1.00', '1616597c9cf8b4b6d50d7cbf3aeb3677efba6587': '1.00',
    '1d949b215bb036fe685511aca4f560d63dd50442': '1.00', 'fd024eaeb58c4b778613b54a9e69f6fbc8615a68': '1.02',
    'de8ce6e778f39b3c2914fd71f1df7117282f62e2': '1.02', 'fed1f511d51613df108101b8a09397beebc70f0c': '1.02',
    '855ac7a96de3e21587fd01e458baeb53c488fa71': '1.00', 'c8b4c0c0c2485246c5373e7445af12d9c4f639a7': '1.00',
    '783e53a7ca38aaa5da74bbd32b00e1891020f86a': '1.00', 'd81e699fac808ebd4a1689c108b9a75571c3d62b': '1.02',
    '5c6e20a2296de593635c585c941e2455b10d7c5f': '1.02', '6b66880c4ad373915f939af62194a71d3b56800d': '1.02',
}
# Where the shaders sit in each known game's files (shaders/<file>), by title id and game code. The data files differ
# between versions and regions, so a pair without an index gets no shader preparation (it would read all its files
# first: many minutes on a hard drive).
SHADER_INDEXES = {
    ('CUSA02343', '1.00'): 'shader_locations_CUSA02343.txt',
    ('CUSA02343', '1.02'): 'shader_locations_CUSA02343_102.txt',
    ('CUSA02320', '1.00'): 'shader_locations_CUSA02320_100.txt',
    ('CUSA02320', '1.02'): 'shader_locations_CUSA02320.txt',
    ('CUSA02344', '1.02'): 'shader_locations_CUSA02344.txt',
    ('CUSA02826', '1.02'): 'shader_locations_CUSA02826.txt',  # Spanish (EP9000), from the 1.02 package
}
_executable_hashes = {}


def executable_hash(path):
    """SHA-1 of an executable's game code (hashed once per file size and date): its loaded segments when it is a
    signed executable, else the whole file; '' when it cannot be read."""
    import hashlib
    try:
        stat = path.stat()
    except OSError:
        return ''
    key = (str(path), stat.st_size, stat.st_mtime)
    if key not in _executable_hashes:
        digest = hashlib.sha1()
        try:
            with open(path, 'rb') as f:
                head = f.read(0x20)
                if head[:4] == b'\x4f\x15\x3d\x1d' and len(head) == 0x20:
                    count, = struct.unpack_from('<H', head, 0x18)
                    entries = [struct.unpack('<QQQQ', f.read(32)) for _ in range(count)]
                    for props, offset, size, _memsz in entries:
                        if props & 0x800:   # a segment with data in the file
                            f.seek(offset)
                            digest.update(f.read(size))
                else:
                    f.seek(0)
                    for block in iter(lambda: f.read(1 << 20), b''):
                        digest.update(block)
        except (OSError, struct.error):
            return ''
        _executable_hashes[key] = digest.hexdigest()
    return _executable_hashes[key]


def game_executable(s, key):
    """The game's executable (eboot.bin for a game dumped on its own), or None."""
    folder = game_folder(s, key)
    exe = Path(folder) / next(e for k, _t, _s, e in GAMES if k == key)
    if not exe.is_file():
        exe = Path(folder) / 'eboot.bin'
    return exe if exe.is_file() else None


def game_code(s, key):
    """'1.00' or '1.02' for a known build of the game's executable, '' for an unknown one, None without one."""
    exe = game_executable(s, key)
    return GAME_CODE.get(executable_hash(exe), '') if exe else None


def unsupported_version(s, key):
    """The game's version when the shader preparation is not used on this copy (no shader index ships for its title
    and game code), else ''. Advanced 'prep_unknown_versions' lets a known game code under such a title (another
    region's release with the 1.02 code that has no shipped index) be prepared: the game scans its files once and
    keeps the index. An executable whose game code we do not know stays off."""
    info = game_info(game_folder(s, key))
    if not info:
        return ''
    code = game_code(s, key)
    if code is None or (info['id'], code) in SHADER_INDEXES or (code and s.get('prep_unknown_versions')):
        return ''
    return info.get('version', '?')


def needs_index_scan(s, key):
    """True when this copy is prepared through the Advanced override: its game code is known but no shader index ships
    for its title, so the first preparation reads the game files once."""
    info = game_info(game_folder(s, key))
    if not info or not s.get('prep_unknown_versions'):
        return False
    code = game_code(s, key)
    return bool(code) and (info['id'], code) not in SHADER_INDEXES


def known_code_without_index(s, key):
    """True when the Advanced override would turn the shader preparation on for this copy."""
    info = game_info(game_folder(s, key))
    if not info:
        return False
    code = game_code(s, key)
    return bool(code) and (info['id'], code) not in SHADER_INDEXES


def shader_locations(lists, s, key):
    """shaders/<index> for this copy's title and game code, or None."""
    info = game_info(game_folder(s, key))
    name = SHADER_INDEXES.get((info['id'], game_code(s, key))) if info else None
    return lists[0].with_name(name) if name else None


def game_folder(s, key):
    """Where one game is: its own folder (Game page, for a game dumped on its own), else the collection folder."""
    return str(s.get(f'game_dir_{key}') or '').strip() or str(s.get('game_dir') or '').strip()


def game_available(s, key):
    """The game's executable is in its folder (a game dumped on its own may have it as eboot.bin)."""
    folder = game_folder(s, key)
    if not folder:
        return False
    exe = next(e for k, _t, _s, e in GAMES if k == key)
    own = str(s.get(f'game_dir_{key}') or '').strip()
    if (Path(folder) / exe).is_file():
        return True
    # In a collection folder eboot.bin is Uncharted 1, not this game (run.py locate).
    from prepare import collection_folder
    return bool(own and (Path(folder) / 'eboot.bin').is_file() and not collection_folder(folder))


def mouse_sensitivity(s):
    """The saved sensitivity as the game reads it: a number in the slider's range."""
    try:
        return f"{min(MOUSE_SENS_MAX, max(MOUSE_SENS_MIN, float(s.get('mouse_sensitivity')))):.2f}"
    except (TypeError, ValueError):
        return '2.80'


def mouse_ratio(s, key, top=MOUSE_RATIO_MAX):
    """A saved multiplier of the sensitivity (aiming, vertical) as the game reads it: in the slider's range."""
    try:
        return f"{min(top, max(MOUSE_RATIO_MIN, float(s.get(key)))):.2f}"
    except (TypeError, ValueError):
        return f"{float(APP_DEFAULTS.get(key, '1.0')):.2f}"


def vibration_strength(s):
    """Saved vibration percentage, clamped at the launcher/runtime boundary."""
    try:
        return min(100, max(0, int(float(s.get('pad_vibration_strength', 100)))))
    except (TypeError, ValueError):
        return 100


def walk_speed(s):
    """How far the walk key lets the movement keys push the stick (percent, input.ini walk_speed)."""
    try:
        return min(WALK_SPEED_MAX, max(WALK_SPEED_MIN, int(float(s.get('walk_speed', 65)))))
    except (TypeError, ValueError):
        return 65


def controller_probe_path():
    """The packaged or development loader, which also provides the lightweight controller probe."""
    configured = os.environ.get('BB_PROBE')
    candidates = ([Path(configured)] if configured else []) + [PORT_DIR / 'bin' / 'bb-probe.exe',
                                                               PORT_DIR / 'out' / 'bb-probe.exe']
    return next((path for path in candidates if path.is_file()), None)


def controller_probe_environment(probe):
    """Development builds find SDL beside MSYS2; packages already keep it beside bb-probe."""
    env = dict(os.environ)
    if not (probe.parent / 'SDL3.dll').is_file():
        clang64 = Path(env.get('MSYS2_ROOT', r'C:\msys64')) / 'clang64' / 'bin'
        if clang64.is_dir():
            env['PATH'] = f'{clang64}{os.pathsep}{env.get("PATH", "")}'
    return env


def parse_controller_probe(text):
    """Parse bb-probe's deliberately small key=value controller response."""
    values = {}
    for line in text.splitlines():
        if '=' in line:
            key, value = line.split('=', 1)
            values[key.strip()] = value.strip()
    return {'connected': values.get('connected') == '1',
            'name': values.get('name', ''),
            'rumble': values.get('rumble') == '1',
            'tested': values.get('tested') == '1'}


def side_buttons():
    """(Mouse 4, Mouse 5) held right now: the system's XBUTTON1 / XBUTTON2 (virtual keys 5 and 6)."""
    try:
        return tuple(bool(ctypes.windll.user32.GetAsyncKeyState(vk) & 0x8000) for vk in (5, 6))
    except (AttributeError, OSError):
        return (False, False)


def display_mode():
    """(width, height, refresh rate in Hz) of the main display, or None."""
    try:
        class DevMode(ctypes.Structure):
            _fields_ = [('name', ctypes.c_wchar * 32), ('spec', ctypes.c_ushort), ('driver', ctypes.c_ushort),
                        ('size', ctypes.c_ushort), ('extra', ctypes.c_ushort), ('fields', ctypes.c_ulong),
                        ('position', ctypes.c_long * 2), ('orientation', ctypes.c_ulong),
                        ('fixed', ctypes.c_ulong), ('color', ctypes.c_short * 5), ('form', ctypes.c_wchar * 32),
                        ('logpixels', ctypes.c_ushort), ('bits', ctypes.c_ulong), ('width', ctypes.c_ulong),
                        ('height', ctypes.c_ulong), ('flags', ctypes.c_ulong), ('frequency', ctypes.c_ulong),
                        ('rest', ctypes.c_ulong * 8)]
        mode = DevMode()
        mode.size = ctypes.sizeof(DevMode)
        if ctypes.windll.user32.EnumDisplaySettingsW(None, -1, ctypes.byref(mode)):
            return int(mode.width), int(mode.height), int(mode.frequency)
    except (AttributeError, OSError):
        pass
    return None


def display_refresh_rate():
    """Refresh rate of the main display in Hz, or None."""
    mode = display_mode()
    return mode[2] if mode and mode[2] > 1 else None


def output_size(s):
    """The size the frame is shown at: the screen in fullscreen, else the window."""
    mode = display_mode() if s.get('fullscreen') else None
    if mode:
        return mode[0], mode[1]
    try:
        width, height = (int(v) for v in str(s.get('window_size') or '1920x1080').split('x'))
        return width, height
    except ValueError:
        return 1920, 1080


# The graphics card's largest video memory heap in MiB (bb-gpu-capabilities), 0 while not known.
# Cards up to 6 GB count as small: at 2560 x 1440 a 4 GB card went past its memory budget and the
# game fell to about half speed now and then (2026-10-26); 1920 x 1080 fit.
GPU_VRAM_MIB = 0
SMALL_CARD_MIB = 7168


def detect_vram():
    """The largest device-local heap of the card the game uses, in MiB (0: unknown or integrated)."""
    exe = PORT_DIR / 'bin' / 'bb-gpu-capabilities.exe'
    if not exe.is_file():
        exe = PORT_DIR / 'out' / 'bb-gpu-capabilities.exe'
    try:
        result = subprocess.run([str(exe), '--live-resolution'], capture_output=True, text=True,
                                timeout=30, creationflags=NO_WINDOW)
    except (OSError, subprocess.TimeoutExpired):
        return 0
    for line in result.stderr.splitlines():
        match = re.match(r'GPU: .+, (discrete|integrated or other), (\d+) MiB', line)
        if match:
            return int(match.group(2)) if match.group(1) == 'discrete' else 0
    return 0


def small_card():
    return 0 < GPU_VRAM_MIB < SMALL_CARD_MIB


def render_sizes(s, start=None):
    """('WxH', 'WxH'): the render size (BB_RENDER_RES) and the output size it is
    upscaled to (BB_OUTPUT_RES), or None for the game's own 1920 x 1080."""
    choice = str(s.get('render_resolution') or '1080')
    if choice == 'auto':
        # Automatic: native, but the game's own 1920 x 1080 on a small card.
        choice = '1080' if small_card() else 'native'
        if choice == '1080':
            return None
    limit = RENDER_PIXELS_MAX_BIG if choice in RENDER_4K or choice in RENDER_4K_UPSCALED else RENDER_PIXELS_MAX
    out_w, out_h = output_size(s)
    if choice in RENDER_4K:
        # Drawn at that size and output at it too: the presenter scales the frame to the window
        # (above the window's size, supersampling). Exactly the sizes tested (2160 is not a
        # multiple of 32, but its bloom chain was fine).
        return choice, choice
    if choice in RENDER_4K_UPSCALED:
        out_w, out_h = (3840, 1600) if out_w / max(1, out_h) > 2.0 else (3840, 2160)
        scale = RENDER_4K_UPSCALED[choice]
        width, height = out_w / scale, out_h / scale
    elif choice in SUPERSAMPLED:
        screen_w, screen_h = out_w, out_h
        factor = min(1.5, (SUPERSAMPLED_PIXELS_MAX / (screen_w * screen_h)) ** 0.5)
        # The screen's shape kept exactly (1080p: 2880 x 1620; a rounder height letterboxed it by a few rows).
        out_w, out_h = int(screen_w * factor) // 8 * 8, int(round(screen_h * factor / 2)) * 2
        width, height = screen_w * SUPERSAMPLED[choice], screen_h * SUPERSAMPLED[choice]
    elif choice in RENDER_SCALES:
        scale = RENDER_SCALES[choice]
        width, height = out_w / scale, out_h / scale
    else:
        try:
            width, height = (int(v) for v in choice.split('x'))  # an older fixed size
        except ValueError:
            return None
    if width * height > limit:
        shrink = (limit / (width * height)) ** 0.5
        width, height = width * shrink, height * shrink
    # The game asserts width % 16 == 0; its bloom chain needs a height that halves cleanly
    # (2016x848: its bloom downsample chain turned treasure glints into big white diamonds,
    # 2016x864 was fine): nearest multiple of 32.
    height = 1080 if abs(height - 1080) < 16 else max(32, int(round(height / 32)) * 32)  # 1080: the game's own
    width = int(width) // 16 * 16
    if width < 640 or height < 360 or (width, height, out_w, out_h) == (1920, 1080, 1920, 1080):
        return None
    return f'{width}x{height}', f'{out_w}x{out_h}'


def render_resolution(s):
    """BB_RENDER_RES, or None."""
    sizes = render_sizes(s)
    return sizes[0] if sizes else None


FSR4_DIR = PORT_DIR / 'fsr4_shaders'


def fsr4_tools():
    """The folder with spirv-cross.exe and glslangValidator.exe: the package's bin, else (developers) MSYS2's
    CLANG64 or the PATH. The Vulkan SDK's spirv-cross 1.4.341 cannot decompile the passes ('Cannot resolve
    expression type'); the package ships MSYS2's."""
    for folder in (PORT_DIR / 'bin', Path('C:/msys64/clang64/bin')):
        if (folder / 'spirv-cross.exe').is_file() and (folder / 'glslangValidator.exe').is_file():
            return folder
    found = [shutil.which(name) for name in ('spirv-cross', 'glslangValidator')]
    return Path(found[0]).parent if all(found) and Path(found[0]).parent == Path(found[1]).parent else None


def fsr411_assets():
    """FSR 4.1.1 needs its model assets, built from AMD's DLL (docs/upscaler.md)."""
    return bool(os.environ.get('BB_FSR411_DIR')) or any(
        (folder / 'fsr4_411').is_dir() for folder in (PORT_DIR, DATA_DIR))


def xess_available():
    """Intel XeSS needs Intel's libxess.dll next to bb-probe.exe (packaging/windows/fetch_xess.sh)."""
    override = os.environ.get('BB_XESS_DLL')
    if override:
        return Path(override).is_file()
    return any((PORT_DIR / folder / 'libxess.dll').is_file() for folder in ('bin', 'out'))


def input_ini_path():
    return Path(os.environ.get('BB_INPUT_CONFIG', DATA_DIR / 'input.ini'))


def key_bindings(s):
    """Every action's keys ("A|B"): the player's own, else the defaults; a late action's default key that
    the player already gave to something else stays theirs (the late action is left unbound)."""
    saved = s.get('key_bindings') or {}
    keys = {**DEFAULT_KEYS, **saved}
    taken = {k.strip() for a, v in keys.items() if a not in LATE_ACTIONS or a in saved for k in v.split('|')}
    for action in LATE_ACTIONS:
        if action not in saved and DEFAULT_KEYS[action] in taken:
            keys[action] = ''
    return keys


# The first line of input.ini when the game's in-game menu wrote it (its Controls page, gpu/shim/bbport_overlay.cpp).
GAME_CONTROLS_HEADER = "# Uncharted controls (written by the game's in-game menu, Controls page)"
GAME_CONTROLS_VALUES = ('mouse_sensitivity', 'mouse_aim_sensitivity', 'mouse_vertical', 'mouse_camera_vertical', 'walk_speed')


def import_game_controls(s):
    """The in-game menu's Controls page writes input.ini itself (keys, mouse settings): those settings are taken into the
    launcher's, so that the next write_input_ini keeps them. Returns the settings taken ([] when the launcher wrote the
    file last)."""
    try:
        lines = input_ini_path().read_text(encoding='utf-8').splitlines()
    except (OSError, UnicodeDecodeError):
        return []
    if not lines or lines[0].strip() != GAME_CONTROLS_HEADER:
        return []
    keys = dict(s.get('key_bindings') or {})
    taken = []
    for line in lines[1:]:
        key, sep, value = line.partition('=')
        key, value = key.strip(), value.strip()
        if not sep:
            continue
        if key.startswith('key.') and key[4:] in DEFAULT_KEYS:
            if value == DEFAULT_KEYS[key[4:]]:
                keys.pop(key[4:], None)
            else:
                keys[key[4:]] = value
        elif key in ('mouse_look', 'mouse_invert_y'):
            s[key] = value == '1'
            taken.append(key)
        elif key in GAME_CONTROLS_VALUES:
            s[key] = value
            taken.append(key)
    s['key_bindings'] = keys
    return taken + ['key_bindings']


def write_input_ini(s):
    """input.ini: the keyboard, mouse and controller bindings the game reads at start."""
    keys = key_bindings(s)
    pad = {**DEFAULT_PAD, **(s.get('pad_bindings') or {})}
    lines = ['# Uncharted controls (written by the launcher, Controls page)',
             f"mouse_look={1 if s.get('mouse_look', True) else 0}",
             f"mouse_sensitivity={mouse_sensitivity(s)}",
             f"mouse_aim_sensitivity={mouse_ratio(s, 'mouse_aim_sensitivity')}",
             f"mouse_vertical={mouse_ratio(s, 'mouse_vertical')}",
             f"mouse_camera_vertical={mouse_ratio(s, 'mouse_camera_vertical', CAMERA_VERTICAL_MAX)}",
             f"mouse_invert_y={1 if s.get('mouse_invert_y') else 0}",
             f"pad_invert_y={1 if s.get('pad_invert_y') else 0}",
             f"pad_vibration={1 if s.get('pad_vibration', True) else 0}",
             f"pad_vibration_strength={vibration_strength(s)}",
             f"walk_speed={walk_speed(s)}"]
    lines += [f'key.{action}={keys.get(action, "")}' for action in DEFAULT_KEYS]
    lines += [f'pad.{action}={pad.get(action, "none")}' for action in DEFAULT_PAD]
    path = input_ini_path()
    path.parent.mkdir(parents=True, exist_ok=True)
    write_atomic(path, '\n'.join(lines) + '\n')
    return path


def game_environment_frame_rate(s):
    """The rate the games run at (BB_VBLANK_HZ)."""
    try:
        return max(1, int(s.get('frame_rate') or 60))
    except ValueError:
        return 60


def game_frame_cap(s):
    """BB_FPS_LIMIT for the Frame rate limit, or 0 to leave it unset. The port's limit counts the
    game's own frames and lets them through as soon as they are ready, apart from the vblank: the
    option counts the frames shown, generated ones included, and only ever lowers the rate."""
    try:
        limit = int(s.get('fps_limit') or 0)
    except ValueError:
        return 0
    if limit <= 0:
        return 0
    multiplier = 1
    if str(s.get('frame_generation') or 'off') != 'off':
        try:
            multiplier = max(1, int(s.get('frame_gen_multiplier') or 2))
        except ValueError:
            multiplier = 2
    cap = max(1, round(limit / multiplier))
    return cap if cap < game_environment_frame_rate(s) else 0


def game_environment(s, start=None):
    env = dict(os.environ)
    try:
        import_game_controls(s)   # (the in-game menu changed them since: kept)
        env['BB_INPUT_CONFIG'] = str(write_input_ini(s))
    except OSError:
        pass
    start = start or s['start']
    # The collection folder; with none, the folder of the game started. A game with a folder of its own
    # (Game page) is taken from it, also when another game of the collection starts it.
    env['BB_GAME_DIR'] = s['game_dir'] or game_folder(s, start)
    for key, _t, _s, _e in GAMES:
        if str(s.get(f'game_dir_{key}') or '').strip():
            env[f'BB_GAME_DIR_{key.upper()}'] = game_folder(s, key)
    env['BB_EXE'] = start
    # Drake's Fortune shares eboot.bin with the collection menu: start at its own title instead
    # (extra_env BB_SKIP_COLLECTION_MENU=0 keeps the menu).
    if env['BB_EXE'] == 'u1':
        env['BB_SKIP_COLLECTION_MENU'] = '1'
    if s['user_dir']:
        env['BB_USER_DIR'] = s['user_dir']
    env['BB_LANGUAGE'] = s['language']
    if str(s['player_name']).strip():
        env['BB_USER_NAME'] = str(s['player_name']).strip()
    env['BB_FULLSCREEN'] = '1' if s['fullscreen'] else '0'
    # The game stands still while its window is minimized or out of focus and opens its pause menu on return
    # (an option, off by default).
    env['BB_PAUSE_ON_FOCUS_LOST'] = '1' if s.get('pause_in_background') else '0'
    env['BB_PRESENT_MODE'] = s['present_mode']
    env['BB_VBLANK_HZ'] = str(s.get('frame_rate') or '60')
    cap = game_frame_cap(s)
    if cap:
        env['BB_FPS_LIMIT'] = str(cap)
    pacing = FRAME_PACING_ENV.get(str(s.get('frame_pacing') or 'off'))
    for key, value in pacing or ():
        env[key] = value
    env['BB_WINDOW_SIZE'] = str(s.get('window_size') or '1920x1080')
    sizes = render_sizes(s, start)
    if sizes:
        # The upscaling hooks (BB_OUTPUT_RES) work in all three games: Uncharted 1 and 2 draw
        # their final composite, HUD-less frame and HUD with Uncharted 3's shaders. They are
        # also what runs the anti-aliasing at native: without BB_OUTPUT_RES the older path
        # only works at the games' own 1920x1080, so at any other size it never ran (U1 and U2
        # at Native had no anti-aliasing at all, only the sub-pixel jitter).
        env['BB_RENDER_RES'], env['BB_OUTPUT_RES'] = sizes
    env['BB_ULTRAWIDE'] = str(s.get('ultrawide') or 'auto')
    env['BB_FOV'] = str(s.get('fov') or '100')
    if not s.get('bloom', True):
        env['BB_BLOOM'] = '0'
    # HDR10 output (the renderer falls back to SDR, and logs why, without an HDR10 display).
    if str(s.get('hdr') or 'off') == 'on':
        env['BB_HDR'] = '1'
        env['BB_HDR_PEAK_NITS'] = str(s.get('hdr_peak_nits') or '1000')
        env['BB_HDR_PAPER_WHITE'] = str(s.get('hdr_paper_white') or '203')
        tone = str(s.get('hdr_tone') or '2')
        env['BB_HDR_SCENE_MODE'] = tone if tone in ('0', '1', '2') else '2'
    env['BB_DOF'] = str(s.get('depth_of_field') or '1')
    # The logo movie (off by default). The faster boot (the vblank runs fast from the shader preparation's end until
    # the title, paused while the logos play) is on for everyone in the port itself (BB_FAST_BOOT=0: off).
    if s.get('skip_intro', False):
        env['BB_SKIP_INTRO'] = '1'
    if s.get('unlock_chapters', False):
        env['BB_UNLOCK_CHAPTERS'] = '1'
    if s.get('unlock_rewards', False):
        env['BB_UNLOCK_REWARDS'] = '1'
    if s.get('skip_friends_notice', True):
        env['BB_OFFLINE_NPID'] = '1'
    job_threads = str(s.get('job_threads') or 'auto')
    if job_threads != 'auto':
        env['BB_JOB_SLEEP'] = '0' if job_threads == 'spin' else '1'
    env['BB_ANISO'] = str(s.get('anisotropy') or '0')
    if s.get('full_res_textures', False):
        env['BB_FULL_RES_TEXTURES'] = '1'
    if s.get('hires_shadows', False):
        env['BB_HIRES_SHADOWS'] = '1'
    env['BB_FSR1'] = '1' if s.get('scaler', 'fsr1') == 'fsr1' else '0'
    sharpening = str(s.get('sharpening') or 'off')
    env['BB_RCAS'] = '0' if sharpening == 'off' else '1'
    if sharpening != 'off':
        env['BB_RCAS_ATTENUATION'] = sharpening
    env['BB_UPSCALER'] = str(s.get('anti_aliasing') or 'off')
    if str(s.get('dlss_model') or 'default') != 'default':
        env['BB_DLSS_PRESET'] = str(s.get('dlss_model'))
    env['BB_FSR_SHARPNESS'] = str(s.get('aa_sharpness') or '0')
    env['BB_FRAME_GEN'] = str(s.get('frame_generation') or 'off')
    env['BB_FRAME_GEN_MULTIPLIER'] = str(s.get('frame_gen_multiplier') or '2')
    env['BB_LOW_LATENCY'] = str(s.get('low_latency') or '0')
    if str(s.get('speakers') or 'auto') != 'auto':
        env['BB_SPEAKERS'] = str(s['speakers'])
    env['BB_BUTTON_PROMPTS'] = str(s.get('button_prompts') or 'auto')
    env['BB_XBOX_STYLE'] = 'bilal' if s.get('xbox_style') == 'bilal' else 'antik'
    # The controller also while another window is in front (runtime_pad.c; keyboard and mouse never).
    if s.get('pad_background', False):
        env['BB_PAD_BACKGROUND'] = '1'
    # The in-game menu (Insert) speaks the launcher's language.
    env['BB_UI_LANGUAGE'] = uncharted_i18n.language_code(str(s.get('ui_language') or ''))
    env['BB_CHEATS'] = ','.join([f'{game}:{cheat}' for game, _t, _s, _e in GAMES for cheat in ('health', 'ammo')
                                 if s.get(f'{game}_infinite_{cheat}')] +
                                # without its debug text: devmode_quiet (the same flags, the text block skipped)
                                [f'{game}:devmode' + ('' if s.get(f'{game}_devmode_text', True) else '_quiet')
                                 for game, _t, _s, _e in GAMES if s.get(f'{game}_devmode')] +
                                [f'{game}:{cheat}' for game, cheats in SWITCH_CHEATS.items() for cheat in cheats
                                 if s.get(f'{game}_{cheat}')])
    # Developer mode also opens Naughty Dog's menus, which the games build but never open: F11 the Dev Menu, F8 the
    # Quick Menu (runtime_pad.c).
    if any(s.get(f'{game}_devmode') for game, _t, _s, _e in GAMES):
        env['BB_DEV_MENU'] = '1'
    # Shader preparation: the game builds its shader list once, before play (a full-screen page) or behind it.
    lists = shader_list(start)
    preparation = str(s.get('shader_preparation') or 'before')
    if unsupported_version(s, start):
        preparation = 'off'
    if lists and preparation != 'off':
        env['BB_VARIANT_SHIP'] = str(lists[0])
        if lists[1].is_file():
            env['BB_KEY_LIST'] = str(lists[1])
        # Where the shaders sit in this release's files: no scan of the whole game folder on the first preparation.
        locations = shader_locations(lists, s, start)
        if locations and locations.is_file():
            env['BB_SHADER_LOCATIONS'] = str(locations)
        # The texture (de)tiling pipelines the game uses, built ahead (shaders/tiling_pipelines_<game>.txt).
        tiling = lists[0].with_name(f'tiling_pipelines_{start}.txt')
        if tiling.is_file():
            env['BB_TILING_LIST'] = str(tiling)
        # The compute pipelines (shaders/<game>_compute_keys.txt: one hash per line).
        compute = lists[0].with_name(f'{start}_compute_keys.txt')
        if compute.is_file():
            env['BB_COMPUTE_LIST'] = str(compute)
        if preparation == 'background':
            env['BB_PREP_SCREEN'] = '0'
        elif str(s.get('preparation_speed') or 'all') == 'half':
            env['BB_SHIP_THREADS'] = str(max(1, (os.cpu_count() or 2) // 2))
    for item in str(s['extra_env']).split():
        if '=' in item:
            key, value = item.split('=', 1)
            env[key] = value
    env['PYTHONUNBUFFERED'] = '1'
    env['PYTHONIOENCODING'] = 'utf-8'
    env['PYTHONUTF8'] = '1'  # open() and the pipes must not fall back to the ANSI code page (GBK, ...)
    return env


# ---------------------------------------------------------------------------------------------
# Surfaces: an old leather journal by lamplight: dark warm browns, parchment text, brass accent.

BG, PANEL, SURFACE, CARD, LINE = '#0d0a07', '#110d09', '#15100b', '#1e1710', '#352a1d'
TEXT, MUTED, GOLD, GOLD_HI, GOLD_DIM = '#f3e9d4', '#9c8e76', '#e2a64a', '#f4c46e', '#a5762f'
CARD_HI = '#271e15'
COLORS = {'surface': SURFACE, 'card': CARD, 'card_hi': CARD_HI, 'line': LINE, 'text': TEXT, 'muted': MUTED,
          'gold': GOLD, 'gold_hi': GOLD_HI, 'track': '#33281c', 'knob': '#a3957d', 'knob_on': '#fbf1dc',
          'button': '#2a2117', 'button_hi': '#36291c'}
# Sidebar icons (Segoe Fluent Icons on Windows 11, Segoe MDL2 Assets on Windows 10).
NAV_ICONS = {'play': '\ue768', 'game': '\ue7fc', 'display': '\ue7f4', 'controls': '\ue765',
             'cheats': '\ue945', 'trophies': '\ue734', 'advanced': '\ue713', 'log': '\ue8a5'}
# Trophy grades as the console colours them (the platinum cool white-blue, the rest metal).
TROPHY_COLORS = {'P': '#9fd0f5', 'G': GOLD, 'S': '#cfd3da', 'B': '#cf8548'}
RUST, RUST_HI = GOLD, GOLD_HI


def load_settings():
    """The saved settings over the defaults. Settings from before 0.3.6.51 (no 'mouse_camera_vertical' yet) had the old
    mouse look, whose camera turned about 2.8 times as far per count: their sensitivity is scaled to keep that feel,
    the aiming multiplier by 0.6 so aiming stays as it was, and the normal camera's up and down gets 2.0 (the new
    defaults: 2.8, 0.6, 2.0)."""
    saved = load_json(CONFIG_FILE, {})
    if saved and 'mouse_camera_vertical' not in saved:
        def scaled(key, factor, low, high):
            try:
                return f"{min(high, max(low, float(saved[key]) * factor)):.2f}"
            except (KeyError, TypeError, ValueError):
                return None
        if (value := scaled('mouse_sensitivity', 2.8, 0.1, 10.0)) is not None:
            saved['mouse_sensitivity'] = value
        if (value := scaled('mouse_aim_sensitivity', 0.6, MOUSE_RATIO_MIN, MOUSE_RATIO_MAX)) is not None:
            saved['mouse_aim_sensitivity'] = value
        saved['mouse_camera_vertical'] = '2.0'
    settings = {**APP_DEFAULTS, **saved}
    settings['portable'] = CONFIG_FILE == PORTABLE_FILE
    if import_game_controls(settings):   # the in-game menu's Controls page changed them in the last game
        try:
            CONFIG_DIR.mkdir(parents=True, exist_ok=True)
            write_atomic(CONFIG_FILE, json.dumps(settings, indent=2) + '\n')
            write_input_ini(settings)   # (the launcher's again: the change is in its settings now)
        except OSError:
            pass
    return settings


class Launcher:
    def __init__(self, root, tk, ttk, filedialog, messagebox):
        global UI_LANGUAGE
        self.tk, self.ttk, self.filedialog, self.messagebox = tk, ttk, filedialog, messagebox
        self.root = root
        self.app = load_settings()
        UI_LANGUAGE = uncharted_i18n.language_code(self.app.get('ui_language', ''))
        self.vars = {}
        self.process = self.job = None
        self.preparing = False
        self.entered, self.last_problem, self.stopping = False, '', False
        self.output = queue.Queue()
        self.ui_calls = queue.Queue()
        self.banner_key = self.banner_scaled = self.cover_image = None
        self.card_hot = None
        try:
            self.configure_art()
        except Exception:
            pass
        self.info, self.available = None, {}
        self.update_box = None
        self.update_offer = self.update_text = None  # shown again after a language change
        self.updating = False
        self.releases, self.releases_loading = None, False  # GitHub's release list (Advanced > Version), once loaded
        self.backups_mb = None  # the size of versions/ in MB, measured when the Advanced page first shows
        self.reader = None
        self.closed = False
        root.title(f'{NAME} — The Nathan Drake Collection for Windows · by {AUTHOR}')
        root.configure(bg=BG)
        self.dpi = root.winfo_fpixels('1i') / 96.0
        root.geometry(f'{self.px(1180)}x{self.px(860)}')
        root.minsize(self.px(1040), self.px(760))
        self.set_icon()
        self.load_fonts()
        self.style()
        self.build()
        self.show('play')
        root.protocol('WM_DELETE_WINDOW', self.close)
        root.after(100, self.drain_output)
        root.after(150, self.detect_controller)
        # This version's release notes, once (the first start after an update).
        if self.app.get('notes_seen') != VERSION and not self.app.get('notes_hide_after_update'):
            root.after(900, self.show_new_notes)
        threading.Thread(target=self.find_vram, daemon=True).start()
        threading.Thread(target=delete_cleared_caches, daemon=True).start()
        if UPDATES and self.app.get('check_updates', True):
            threading.Thread(target=self.check_update, daemon=True).start()

    def find_vram(self):
        global GPU_VRAM_MIB
        GPU_VRAM_MIB = detect_vram()
        self.ui_calls.put(self.update_vram_note)

    def update_vram_note(self):
        """A note under Render resolution when a small card would draw above 1920 x 1080."""
        note = getattr(self, 'vram_note', None)
        if note is None:
            return
        s = dict(self.app)
        for key in ('render_resolution', 'window_size', 'fullscreen'):
            s[key] = self.var(key).get()
        sizes = render_sizes(s)
        width, height = (int(v) for v in sizes[0].split('x')) if sizes else (1920, 1080)
        text = ''
        if small_card() and width * height > 1920 * 1080:
            text = _('Your graphics card has {} GB of video memory: above 1920 × 1080 the game may not '
                     'fit in it and can run at half speed or stutter. 1920 × 1080, Automatic or '
                     'Performance is recommended; FSR needs about 350 MB less video memory than '
                     'DLSS.').format(round(GPU_VRAM_MIB / 1024))
        note.configure(text=text)

    def px(self, size):
        return int(size * self.dpi)

    def set_icon(self):
        try:
            self.root.iconbitmap(default=str(PORT_DIR / 'launcher' / 'uncharted.ico'))
        except self.tk.TclError:
            pass

    def load_fonts(self):
        """Cinzel for headings (Uncharted-style capitals); Segoe UI for body text."""
        from uncharted_fonts import font_path

        self.brand_family = 'Cinzel'
        for name in ('Cinzel-Regular.ttf', 'Cinzel-SemiBold.ttf', 'Cinzel-Bold.ttf'):
            path = font_path(name)
            if path:
                try:
                    ctypes.windll.gdi32.AddFontResourceExW(str(path), 0x10, 0)
                except (AttributeError, OSError):
                    pass
        if not font_path('Cinzel-Bold.ttf'):
            self.brand_family = 'Georgia'
        self.font_brand_xl = (self.brand_family, 24, 'bold')
        self.font_brand_lg = (self.brand_family, 18, 'bold')
        self.font_brand_md = (self.brand_family, 13, 'bold')
        self.font_brand_sm = (self.brand_family, 10)
        self.font_ui = ('Segoe UI', 10)
        self.font_ui_sm = ('Segoe UI', 9)
        self.font_ui_md = ('Segoe UI Semibold', 10)
        self.font_button = ('Segoe UI Semibold', 10)
        self.font_play = ('Segoe UI Semibold', 13)
        self.font_title = ('Segoe UI Semibold', 22)
        self.font_card = ('Segoe UI Semibold', 9)
        import tkinter.font as tkfont
        families = set(tkfont.families(self.root))
        icons = next((f for f in ('Segoe Fluent Icons', 'Segoe MDL2 Assets') if f in families), None)
        self.font_icon = (icons, 13) if icons else None
        self.font_icon_sm = (icons, 11) if icons else None

    # ---- look --------------------------------------------------------------------------------
    def check_images(self):
        """16 px (DPI-scaled) check box images: the clam theme draws a cross."""
        tk = self.tk
        n = max(14, int(16 * self.dpi))
        images = []
        for checked in (False, True):
            image = tk.PhotoImage(width=n + self.px(8), height=n)
            image.put(GOLD if checked else '#6a5c40', to=(0, 0, n, n))
            image.put('#1a1408' if checked else CARD, to=(1, 1, n - 1, n - 1))
            if checked:
                x0, y0, x1, y1, x2, y2 = .22 * n, .52 * n, .42 * n, .72 * n, .78 * n, .28 * n
                width = max(1, round(n / 9))
                for t in range(40):
                    f = t / 39
                    for x, y in ((x0 + (x1 - x0) * f, y0 + (y1 - y0) * f), (x1 + (x2 - x1) * f, y1 + (y2 - y1) * f)):
                        image.put(GOLD, to=(int(x), int(y), int(x) + width, int(y) + width))
            images.append(image)
        self.check_off, self.check_on = images

    def style(self):
        s = self.ttk.Style(self.root)
        s.theme_use('clam')
        self.check_images()
        s.element_create('Nd.indicator', 'image', self.check_off, ('selected', self.check_on), sticky='')
        s.layout('TCheckbutton', [('Checkbutton.padding', {'sticky': 'nswe', 'children': [
            ('Nd.indicator', {'side': 'left', 'sticky': ''}),
            ('Checkbutton.focus', {'side': 'left', 'sticky': 'w', 'children': [
                ('Checkbutton.label', {'sticky': 'nswe'})]})]})])
        base = self.font_ui
        self.root.option_add('*TCombobox*Listbox.background', CARD)
        self.root.option_add('*TCombobox*Listbox.foreground', TEXT)
        self.root.option_add('*TCombobox*Listbox.selectBackground', RUST)
        self.root.option_add('*TCombobox*Listbox.font', base)
        s.configure('.', background=SURFACE, foreground=TEXT, fieldbackground=CARD, bordercolor=LINE,
                    lightcolor=LINE, darkcolor=LINE, troughcolor=CARD, focuscolor=GOLD, font=base)
        s.configure('TFrame', background=SURFACE)
        s.configure('Shell.TFrame', background=SURFACE)
        s.configure('Card.TFrame', background=CARD)
        s.configure('TLabel', background=CARD, foreground=TEXT)
        s.configure('Shell.TLabel', background=SURFACE, foreground=TEXT)
        s.configure('Muted.TLabel', background=CARD, foreground=MUTED, font=('Segoe UI', 9))
        s.configure('Warn.TLabel', background=CARD, foreground=GOLD, font=('Segoe UI', 9))
        s.configure('ShellMuted.TLabel', background=SURFACE, foreground=MUTED, font=('Segoe UI', 9))
        s.configure('Section.TLabel', background=SURFACE, foreground=GOLD_DIM,
                    font=(self.brand_family, 10, 'bold'))
        s.configure('TCheckbutton', background=SURFACE, foreground=TEXT, padding=(0, 3))
        s.map('TCheckbutton', background=[('active', SURFACE)], foreground=[('disabled', MUTED)])
        s.configure('Ghost.TButton', background=CARD, foreground=MUTED, padding=(16, 8), borderwidth=0)
        s.map('Ghost.TButton', background=[('active', CARD)], foreground=[('active', TEXT)])
        s.configure('TCombobox', arrowcolor=GOLD, foreground=TEXT, padding=(8, 5), bordercolor=CARD_HI,
                    lightcolor=CARD_HI, darkcolor=CARD_HI, background=CARD_HI, arrowsize=14)
        s.map('TCombobox', fieldbackground=[('readonly', CARD_HI)], foreground=[('readonly', TEXT)],
              selectbackground=[('readonly', CARD_HI)], selectforeground=[('readonly', TEXT)],
              bordercolor=[('focus', GOLD_DIM)], lightcolor=[('focus', GOLD_DIM)], background=[('active', '#30251a')])
        s.configure('TEntry', foreground=TEXT, insertcolor=TEXT, padding=(8, 5), fieldbackground=CARD_HI,
                    bordercolor=CARD_HI, lightcolor=CARD_HI, darkcolor=CARD_HI)
        s.map('TEntry', bordercolor=[('focus', GOLD_DIM)], lightcolor=[('focus', GOLD_DIM)])
        s.configure('TButton', background='#221f1a', foreground=TEXT, padding=(14, 7), borderwidth=0)
        s.map('TButton', background=[('active', '#322c24'), ('disabled', PANEL)], foreground=[('disabled', MUTED)])
        s.configure('Play.TButton', background=GOLD, foreground='#1a1408', font=('Segoe UI', 13, 'bold'),
                    padding=(42, 12), borderwidth=0)
        s.map('Play.TButton', background=[('active', '#f4c46e'), ('disabled', '#3a3428')],
              foreground=[('disabled', '#8a8070'), ('active', '#1a1408')])
        s.configure('Vertical.TScrollbar', background='#30251a', arrowcolor=MUTED, troughcolor=SURFACE,
                    bordercolor=SURFACE, lightcolor='#30251a', darkcolor='#30251a', gripcount=0, arrowsize=12)
        s.map('Vertical.TScrollbar', background=[('active', '#463624')])
        s.configure('Gold.Horizontal.TProgressbar', background=GOLD, troughcolor=CARD_HI, bordercolor=CARD_HI,
                    lightcolor=GOLD, darkcolor=GOLD_DIM, thickness=self.px(10))

    # ---- widget helpers ----------------------------------------------------------------------
    def var(self, key):
        if key not in self.vars:
            value = self.app.get(key, APP_DEFAULTS[key])
            v = self.tk.BooleanVar(value=bool(value)) if isinstance(APP_DEFAULTS[key], bool) \
                else self.tk.StringVar(value=str(value))
            self.vars[key] = v
        return self.vars[key]

    def choice(self, parent, key, options, width=36, on_change=None):
        var = self.var(key)
        values = [v for v, _t in options]
        box = self.ttk.Combobox(parent, values=[_(t) for _v, t in options], state='readonly', width=width)
        if var.get() not in values:
            var.set(values[0])

        def show(*_args):
            box.current(values.index(var.get()) if var.get() in values else 0)
        show()
        def selected(_event):
            var.set(values[box.current()])
            if on_change:
                on_change()
        box.bind('<<ComboboxSelected>>', selected)
        var.trace_add('write', show)

        def set_options(new_options):
            values[:] = [v for v, _t in new_options]
            box.configure(values=[_(t) for _v, t in new_options])
            show()
        box.set_options = set_options
        return box

    def next_row(self, parent):
        return parent.grid_size()[1]

    def target(self, parent):
        """Where a page's rows go: its current card (one is opened when rows come before a section)."""
        if parent in self.page_cards:
            if self.page_cards[parent] is None:
                self.new_card(parent, None)
            self.page_cards[parent].refit()
            return self.page_cards[parent].inner
        return parent

    def new_card(self, page, title):
        from uncharted_widgets import Card
        card = Card(self.tk, page, COLORS, self.px, title, self.font_card)
        card.canvas.grid(row=self.next_row(page), column=0, columnspan=2, sticky='ew', pady=(0, self.px(14)))
        self.page_cards[page] = card
        return card

    def toggle(self, parent, variable, background=CARD):
        from uncharted_widgets import Toggle
        return Toggle(self.tk, parent, variable, COLORS, self.px, background)

    def pill(self, parent, text, command, kind='secondary', background=CARD, padding=(16, 8), icon=None):
        from uncharted_widgets import PillButton
        return PillButton(self.tk, parent, text, command, COLORS, self.px,
                          self.font_play if kind == 'primary' and icon else self.font_button, kind,
                          background, padding, icon, self.font_icon_sm)

    def row(self, parent, title, widget, hint=None):
        parent = self.target(parent)
        r = self.next_row(parent)
        self.ttk.Label(parent, text=title).grid(row=r, column=0, sticky='nw', padx=(0, 24), pady=(9, 0))
        widget.grid(row=r, column=1, sticky='w', pady=(5, 0), in_=parent)
        widget.lift()
        if hint:
            self.ttk.Label(parent, text=hint, style='Muted.TLabel', wraplength=self.px(560), justify='left').grid(
                row=r + 1, column=1, sticky='w', pady=(3, 4))
        return widget

    def check(self, parent, key, title, hint=None, indent=0):
        parent = self.target(parent)
        r = self.next_row(parent)
        line = self.tk.Frame(parent, bg=CARD)
        line.grid(row=r, column=0, columnspan=2, sticky='w', pady=(7, 0), padx=(self.px(indent), 0))
        self.toggle(line, self.var(key)).pack(side='left')
        label = self.tk.Label(line, text=title, bg=CARD, fg=TEXT, font=self.font_ui, cursor='hand2')
        label.pack(side='left', padx=(self.px(12), 0))
        label.bind('<Button-1>', lambda _e: self.var(key).set(not self.var(key).get()))
        if hint:
            self.ttk.Label(parent, text=hint, style='Muted.TLabel', wraplength=self.px(640), justify='left').grid(
                row=r + 1, column=0, columnspan=2, sticky='w', padx=(self.px(52), 0), pady=(2, 2))

    def note(self, parent, text, top=12):
        parent = self.target(parent)
        self.ttk.Label(parent, text=text, style='Muted.TLabel', wraplength=self.px(680), justify='left').grid(
            row=self.next_row(parent), column=0, columnspan=2, sticky='w', pady=(top, 0))

    def section(self, parent, title, top=18):
        if parent in self.page_cards:
            return self.new_card(parent, title).inner
        self.ttk.Label(parent, text=title, style='Section.TLabel').grid(
            row=self.next_row(parent), column=0, columnspan=2, sticky='w', pady=(top, 2))
        return parent

    def folder(self, parent, key, title, prompt, hint=None, on_change=None):
        var = self.var(key)
        holder = self.tk.Frame(self.target(parent), bg=CARD)
        self.ttk.Entry(holder, textvariable=var, width=54).pack(side='left')

        def browse():
            chosen = self.filedialog.askdirectory(title=prompt, initialdir=var.get() or str(PORT_DIR))
            if chosen:
                var.set(str(Path(chosen)))
        self.pill(holder, _('Browse…'), browse, padding=(14, 6)).pack(side='left', padx=(8, 0))
        self.pill(holder, _('Open'), lambda: self.open_path(var.get(), key), kind='ghost',
                  padding=(12, 6)).pack(side='left', padx=(4, 0))
        if on_change:
            var.trace_add('write', lambda *_a: on_change())
        return self.row(parent, title, holder, hint)

    def page(self, name, title, subtitle):
        tk = self.tk
        outer = tk.Frame(self.content, bg=SURFACE)
        shell = tk.Frame(outer, bg=SURFACE)
        shell.pack(fill='both', expand=True, padx=self.px(18), pady=(self.px(14), 0))
        head = tk.Frame(shell, bg=SURFACE)
        head.pack(fill='x', padx=self.px(26), pady=(self.px(18), self.px(14)))
        tk.Label(head, text=title, bg=SURFACE, fg=TEXT, font=self.font_title).pack(anchor='w')
        tk.Label(head, text=subtitle, bg=SURFACE, fg=MUTED, font=self.font_ui).pack(anchor='w', pady=(2, 0))
        # The page body scrolls when it is taller than the window (Display holds many options).
        body = tk.Frame(shell, bg=SURFACE)
        body.pack(fill='both', expand=True)
        canvas = tk.Canvas(body, bg=SURFACE, highlightthickness=0, bd=0)
        bar = self.ttk.Scrollbar(body, orient='vertical', command=canvas.yview)
        canvas.configure(yscrollcommand=bar.set)
        canvas.pack(side='left', fill='both', expand=True)
        inner = self.ttk.Frame(canvas, style='Shell.TFrame', padding=(26, 0, 26, 12))
        inner.columnconfigure(1, weight=1)
        self.page_cards[inner] = None
        window = canvas.create_window(0, 0, window=inner, anchor='nw')

        def fit(_event=None):
            canvas.itemconfigure(window, width=canvas.winfo_width())
            canvas.configure(scrollregion=(0, 0, canvas.winfo_width(), inner.winfo_reqheight()))
            if inner.winfo_reqheight() > canvas.winfo_height():
                bar.pack(side='right', fill='y')
            else:
                bar.pack_forget()
                canvas.yview_moveto(0)
        inner.bind('<Configure>', fit)
        canvas.bind('<Configure>', fit)

        self.scrollers[name] = (canvas, bar)
        self.pages[name] = outer
        return inner

    def on_wheel(self, event):
        """The mouse wheel scrolls the page under the pointer (also over its buttons and lists;
        lists do not change their choice); while a control is being bound it is the binding."""
        if getattr(self, 'capturing', None):
            return self.captured_wheel(event)
        scroller = self.scrollers.get(self.current_page)
        if scroller:
            canvas, bar = scroller
            x, y = self.root.winfo_pointerxy()
            inside = (canvas.winfo_rootx() <= x < canvas.winfo_rootx() + canvas.winfo_width() and
                      canvas.winfo_rooty() <= y < canvas.winfo_rooty() + canvas.winfo_height())
            if inside and bar.winfo_ismapped():
                canvas.yview_scroll(-1 * (event.delta // 120), 'units')
        return 'break'

    # ---- layout ------------------------------------------------------------------------------
    def build(self):
        tk, ttk = self.tk, self.ttk
        side_w = self.px(248)
        side = tk.Frame(self.root, bg=BG, width=side_w)
        side.pack(side='left', fill='y')
        side.pack_propagate(False)
        brand_wrap = side_w - 36
        self.emblem_image = None
        try:
            from PIL import ImageTk
            import uncharted_art
            mark = uncharted_art.emblem(self.px(64))
            self.emblem_image = ImageTk.PhotoImage(mark) if mark else None
        except Exception:
            self.emblem_image = None
        if self.emblem_image:
            tk.Label(side, image=self.emblem_image, bg=BG, bd=0).pack(anchor='w', padx=14, pady=(22, 0))
        tk.Label(side, text='UNCHARTED', bg=BG, fg=GOLD, font=self.font_brand_lg, anchor='w').pack(
            anchor='w', padx=18, pady=(10 if self.emblem_image else 28, 0))
        tk.Label(side, text='NATHAN DRAKE', bg=BG, fg=MUTED, font=self.font_brand_sm, anchor='w').pack(
            anchor='w', padx=18, pady=(8, 0))
        tk.Label(side, text='COLLECTION', bg=BG, fg=MUTED, font=self.font_brand_sm, anchor='w').pack(
            anchor='w', padx=18, pady=(1, 0))
        tk.Label(side, text=_('by {}').format(AUTHOR), bg=BG, fg=TEXT, font=self.font_ui_md,
                 anchor='w', wraplength=brand_wrap, justify='left').pack(anchor='w', padx=18, pady=(10, 0))
        version_label = tk.Label(side, text=_('version {}').format(VERSION) + '  \u00b7  ' + _("what's new"),
                                 bg=BG, fg='#6e604c', font=self.font_ui_sm, anchor='w', cursor='hand2')
        version_label.pack(anchor='w', padx=18, pady=(1, 0))
        version_label.bind('<Button-1>', lambda _e: self.show_notes())
        version_label.bind('<Enter>', lambda _e: version_label.configure(fg=GOLD))
        version_label.bind('<Leave>', lambda _e: version_label.configure(fg='#6e604c'))
        tk.Frame(side, bg=LINE, height=1).pack(fill='x', padx=18, pady=(16, 14))
        self.nav, self.nav_pills, self.nav_icons, self.nav_marks, self.current_page = {}, {}, {}, {}, None
        for name, title in (('play', _('Play')), ('game', _('Game')), ('cheats', _('Cheats')), ('display', _('Display')),
                            ('controls', _('Controls')), ('trophies', _('Trophies')), ('advanced', _('Advanced')),
                            ('log', _('Log'))):
            row = tk.Frame(side, bg=BG, cursor='hand2')
            row.pack(fill='x', padx=(0, 12), pady=2)
            mark = tk.Frame(row, bg=BG, width=self.px(3))
            mark.pack(side='left', fill='y')
            pill = tk.Frame(row, bg=BG, cursor='hand2')
            pill.pack(side='left', fill='x', expand=True, padx=(self.px(9), 0))
            widgets = [row, pill]
            if self.font_icon:
                icon = tk.Label(pill, text=NAV_ICONS[name], bg=BG, fg=MUTED, font=self.font_icon,
                                padx=self.px(12), pady=self.px(9), cursor='hand2')
                icon.pack(side='left')
                self.nav_icons[name] = icon
                widgets.append(icon)
            item = tk.Label(pill, text=title, bg=BG, fg=TEXT, anchor='w', font=self.font_ui_md,
                            padx=self.px(2 if self.font_icon else 14), pady=self.px(9), cursor='hand2')
            item.pack(side='left', fill='x', expand=True)
            widgets.append(item)
            for widget in widgets:
                widget.bind('<Button-1>', lambda _e, n=name: self.show(n))
                widget.bind('<Enter>', lambda _e, n=name: self._nav_hover(n, True))
                widget.bind('<Leave>', lambda _e, n=name: self._nav_hover(n, False))
            self.nav[name], self.nav_pills[name], self.nav_marks[name] = item, pill, mark
        tk.Frame(side, bg=BG).pack(fill='both', expand=True)
        self.side = side
        self.side_note = tk.Label(side, text=_('Native port: the original PS4 game code runs directly on Windows.'),
                                  bg=BG, fg=MUTED, font=('Segoe UI', 9), justify='left',
                                  wraplength=side_w - 36)
        self.side_note.pack(anchor='w', padx=18, pady=(0, 18))

        tk.Frame(self.root, bg=LINE, width=1).pack(side='left', fill='y')
        right = tk.Frame(self.root, bg=SURFACE)
        right.pack(side='left', fill='both', expand=True)
        bar = tk.Frame(right, bg=PANEL, height=self.px(78))
        bar.pack(fill='x', side='bottom')
        bar.pack_propagate(False)
        tk.Frame(bar, bg=LINE, height=1).pack(fill='x')
        dock = tk.Frame(bar, bg=PANEL)
        dock.pack(fill='both', expand=True)
        self.status = tk.Label(dock, text='', bg=PANEL, fg=MUTED, font=('Segoe UI', 10), anchor='w', justify='left')
        self.status.pack(side='left', padx=24, pady=14)
        # Beside the status after the game stopped with an error (drain_once), hidden again at the next start.
        self.crash_log_button = self.pill(dock, _('Open log folder'), self.open_log_folder, background=PANEL,
                                          padding=(16, 6))
        self.play_button = self.pill(dock, _('Play'), self.play, kind='primary', background=PANEL,
                                     padding=(30, 9), icon='\ue768' if self.font_icon else None)
        self.play_button.pack(side='right', padx=(10, 22), pady=12)
        self.stop_button = self.pill(dock, _('Stop'), self.stop, background=PANEL, padding=(24, 9),
                                     icon='\ue71a' if self.font_icon else None)
        self.stop_button.configure(state='disabled')
        self.stop_button.pack(side='right', pady=12)
        self.content = tk.Frame(right, bg=SURFACE)
        self.content.pack(fill='both', expand=True)

        self.pages, self.scrollers, self.page_cards = {}, {}, {}
        self.root.bind_all('<MouseWheel>', self.on_wheel)
        self.root.bind_class('TCombobox', '<MouseWheel>', self.on_wheel)
        self.build_play()
        self.build_game()
        self.build_display()
        self.build_controls()
        self.build_trophies()
        self.build_cheats()
        self.build_advanced()
        self.build_log()
        self.refresh()

    def _nav_style(self, name, active=False, hover=False):
        if active:
            bg, fg, icon = '#1e1710', TEXT, GOLD
        elif hover:
            bg, fg, icon = '#120d09', TEXT, TEXT
        else:
            bg, fg, icon = BG, MUTED, MUTED
        self.nav_pills[name].configure(bg=bg)
        self.nav[name].configure(bg=bg, fg=fg)
        if name in self.nav_icons:
            self.nav_icons[name].configure(bg=bg, fg=icon)
        self.nav_marks[name].configure(bg=GOLD if active else BG)

    def _nav_hover(self, name, inside):
        if name == self.current_page:
            return
        self._nav_style(name, hover=inside)

    def _card_hover(self, key):
        if self.card_hot == key:
            return
        self.card_hot = key
        for name in self.cards:
            self.draw_card(name)

    def show(self, name):
        for page in self.pages.values():
            page.pack_forget()
        self.pages[name].pack(fill='both', expand=True)
        self.current_page = name
        for n in self.nav:
            self._nav_style(n, active=(n == name))
        if name == 'play':
            self.refresh()
        elif name == 'trophies':
            self.trophy_info.clear()  # the game folder or the language may have changed
            self.trophy_state = None
            self.render_trophies()
        elif name == 'advanced' and UPDATES:
            if self.releases is None and not self.releases_loading:
                self.releases_loading = True
                threading.Thread(target=self.load_releases, daemon=True).start()
            self.show_backups_size()

    def build_play(self):
        tk, ttk = self.tk, self.ttk
        page = tk.Frame(self.content, bg=SURFACE)
        self.pages['play'] = page
        self.hero = tk.Canvas(page, height=self.px(260), bg=SURFACE, highlightthickness=0, bd=0)
        self.hero.pack(fill='x')
        self.hero.bind('<Configure>', self._sync_hero_height)
        self.hero.bind('<Button-1>', lambda _e: self.edit_launcher_art(
            'art_banner', _('Collection banner'), (1600, 450), 'banner'))
        self.hero.configure(cursor='hand2')
        shell = tk.Frame(page, bg=SURFACE)
        shell.pack(fill='both', expand=True, padx=self.px(22), pady=(self.px(6), self.px(10)))
        head = tk.Frame(shell, bg=SURFACE)
        head.pack(fill='x', padx=self.px(22), pady=(self.px(14), self.px(8)))
        tk.Label(head, text=_('Library'), bg=SURFACE, fg=TEXT,
                 font=('Segoe UI Semibold', 14)).pack(side='left')
        tk.Label(head, text=_('Double-click a cover to launch'), bg=SURFACE, fg=MUTED,
                 font=self.font_ui_sm).pack(side='right')
        cards = tk.Frame(shell, bg=SURFACE)
        cards.pack(fill='x', padx=self.px(14), pady=(0, self.px(6)))
        self.cards, self.card_images = {}, {}
        for column, (key, _title, _subtitle, _exe) in enumerate(GAMES):
            cards.columnconfigure(column, weight=1, uniform='games')
            card = tk.Canvas(cards, height=self.px(460), bg=SURFACE, highlightthickness=0, bd=0, cursor='hand2')
            card.grid(row=0, column=column, sticky='nsew', padx=(0 if column == 0 else 18, 0))
            card.bind('<Configure>', lambda _e, k=key: self.draw_card(k))
            card.bind('<Button-1>', lambda _e, k=key: self.var('start').set(k))
            card.bind('<Double-Button-1>', lambda _e, k=key: (self.var('start').set(k), self.play()))
            card.bind('<Enter>', lambda _e, k=key: self._card_hover(k))
            card.bind('<Leave>', lambda _e, k=key: self._card_hover(None))
            self.cards[key] = card
        cards.bind('<Configure>', self.fit_posters)
        tk.Frame(shell, bg=SURFACE, height=1).pack(fill='x', padx=self.px(22), pady=(self.px(4), self.px(8)))
        row = tk.Frame(shell, bg=SURFACE)
        row.pack(fill='x', padx=self.px(22), pady=(0, self.px(8)))
        self.toggle(row, self.var('fullscreen'), SURFACE).pack(side='left')
        fullscreen = tk.Label(row, text=_('Fullscreen'), bg=SURFACE, fg=TEXT, font=self.font_ui, cursor='hand2')
        fullscreen.pack(side='left', padx=(self.px(10), 0))
        fullscreen.bind('<Button-1>', lambda _e: self.var('fullscreen').set(not self.var('fullscreen').get()))
        self.pill(row, _('Add to Steam'), self.add_to_steam, background=SURFACE, padding=(14, 6)).pack(
            side='left', padx=(self.px(24), 0))
        self.pill(row, _('Desktop shortcut'), self.shortcut, background=SURFACE, padding=(14, 6)).pack(
            side='left', padx=(self.px(8), 0))
        # The collection's own menu (it lives in Uncharted 1's eboot.bin): pick any of the three games, as on the PS4.
        self.pill(row, _('Collection menu'), lambda: self.play(collection=True), background=SURFACE,
                  padding=(14, 6)).pack(side='left', padx=(self.px(8), 0))
        self.ready = tk.Label(shell, text='', bg=SURFACE, fg=MUTED, font=self.font_ui_sm,
                              wraplength=self.px(820), justify='left')
        self.ready.pack(anchor='w', padx=self.px(22), pady=(0, self.px(16)))
        self.var('start').trace_add('write', lambda *_a: self.refresh())

    def fit_posters(self, _event=None):
        """Poster tiles keep the cover shape, but stay short enough that the caption is on screen."""
        sample = self.cards.get('u1')
        if sample is None:
            return
        width = sample.winfo_width()
        if width < 80:
            return
        page = self.pages.get('play')
        natural = int(round(width * 365 / 273)) + self.px(78)
        avail = natural
        if page is not None and page.winfo_height() > 240:
            # Leave the library heading, the fullscreen row, and the status line on screen.
            avail = page.winfo_height() - self.hero.winfo_height() - self.px(196)
        total = max(self.px(168), min(natural, avail))
        for card in self.cards.values():
            if int(card['height']) != total:
                card.configure(height=total)

    def _tile_content_size(self, canvas_w, canvas_h):
        """Size passed to poster_tile so the bitmap, including its shadow pad, stays inside the card."""
        inner_w = max(48, canvas_w - 8)
        inner_h = max(80, canvas_h - 8)
        while inner_w > 48 and inner_w + 2 * max(10, inner_w // 28) > canvas_w - 2:
            inner_w -= 2
        pad = max(10, inner_w // 28)
        while inner_h > 80 and inner_h + 2 * pad > canvas_h - 2:
            inner_h -= 2
        return inner_w, inner_h

    def draw_card(self, key):
        """Rounded cover tile with shadow; gold ring when selected."""
        card = self.cards[key]
        w, h = card.winfo_width(), card.winfo_height()
        if w < 48 or h < 48:
            return
        title, subtitle = next((t, sub) for k, t, sub, _e in GAMES if k == key)
        present = self.available.get(key, True)
        chosen = key == self.var('start').get()
        hot = key == self.card_hot
        inner_w, inner_h = self._tile_content_size(w, h)
        page = tuple(int(SURFACE[i:i + 2], 16) for i in (1, 3, 5))
        wanted = (inner_w, inner_h, present, chosen, hot)
        if self.card_images.get(key, (None,))[0] != wanted:
            image = None
            try:
                from PIL import ImageTk
                import uncharted_art
                scene = uncharted_art.poster_tile(key, inner_w, inner_h, title, subtitle, selected=chosen,
                                                  hover=hot and not chosen, dimmed=not present, page=page)
                image = ImageTk.PhotoImage(scene)
            except Exception:
                image = None
            self.card_images[key] = (wanted, image)
        card.delete('all')
        image = self.card_images[key][1]
        if image:
            x = max(0, (w - image.width()) // 2)
            card.create_image(x, 0, image=image, anchor='nw')
        else:
            card.create_rectangle(0, 0, w, h, fill=CARD, outline=LINE)
        if hot:
            label = _('Change image')
            box_w = max(self.px(108), len(label) * self.px(7))
            card.create_rectangle(w - box_w - self.px(12), self.px(12), w - self.px(12), self.px(42),
                                  fill='#120d09', outline=GOLD_DIM, tags='edit_art')
            card.create_text(w - box_w // 2 - self.px(12), self.px(27), text=label, fill=TEXT,
                             font=self.font_ui_sm, tags='edit_art')
            card.tag_bind('edit_art', '<Button-1>', lambda _e, k=key: self.edit_launcher_art(
                f'art_{k}', _('Change image'), (600, 900), k))

    def _sync_hero_height(self, _event=None):
        """Keep the header at the banner's 16:9 aspect ratio."""
        c = self.hero
        w = max(c.winfo_width(), 400)
        h = max(self.px(180), min(self.px(280), int(w * 576 / 1024)))
        if int(c['height']) != h:
            c.configure(height=h)
        self.draw_hero()

    def draw_hero(self):
        """Collection banner up top; selected game and folder status along the bottom edge."""
        c = self.hero
        c.delete('all')
        w, h = max(c.winfo_width(), 400), int(c['height'])
        start = self.var('start').get()
        title, subtitle = next((t, sub) for k, t, sub, _e in GAMES if k == start)
        game_dir = game_folder(self.current(), start)
        key = (w, h, start, game_dir, self.app.get('art_banner', ''))
        if self.banner_key != key:
            self.banner_key = key
            # The collection banner (original art that comes with the port), else the scene.
            self.banner_scaled = self.photo_collection_banner(w, h) or self.photo_hero(start, w, h)
            self.cover_image = None
        if self.banner_scaled:
            c.create_image(w // 2, h // 2, image=self.banner_scaled)
        c.create_rectangle(0, h - 1, w, h, fill=GOLD_DIM, outline='')
        info = self.info
        sub = (f"{info['id']}  ·  {_('version')} {info['version']}" if info
               else _('Set your game folder on the Game page'))
        c.create_text(self.px(24), h - self.px(14), text=sub, anchor='sw', fill=GOLD,
                      font=self.font_brand_sm)
        c.create_text(w - self.px(24), h - self.px(14), text=f'{_(title)}  ·  {_(subtitle)}',
                      anchor='se', fill='#f8eedb', font=self.font_brand_md)
        c.create_text(w - self.px(24), self.px(18), text=_('Change image'), anchor='ne', fill='#d6c9ae',
                      font=self.font_ui_sm)

    def configure_art(self):
        import uncharted_art
        crops = self.app.get('art_crops', {})
        uncharted_art.set_custom_art({
            'collection_banner.jpg': self.app.get('art_banner', ''),
            'u1.jpg': self.app.get('art_u1', ''),
            'u2.jpg': self.app.get('art_u2', ''),
            'u3.jpg': self.app.get('art_u3', ''),
        }, {
            'collection_banner.jpg': crops.get('banner', {}),
            'u1.jpg': crops.get('u1', {}),
            'u2.jpg': crops.get('u2', {}),
            'u3.jpg': crops.get('u3', {}),
        })

    def image_path(self, title, current=''):
        """Choose and validate a local image."""
        chosen = self.filedialog.askopenfilename(
            title=title, initialdir=str(Path(current).parent) if current else str(PORT_DIR),
            filetypes=[(_('Image files'), '*.png *.jpg *.jpeg *.webp'), (_('All files'), '*.*')])
        if not chosen:
            return ''
        try:
            from PIL import Image
            with Image.open(chosen) as image:
                image.verify()
        except (OSError, ValueError) as error:
            self.messagebox.showerror(NAME, _('This image could not be opened: {}').format(error))
            return ''
        return str(Path(chosen))

    def edit_art(self, key, title, size, crop_key):
        """Preview, position and save one local image without copying it into the port."""
        from PIL import Image, ImageTk
        import uncharted_art

        window = self.tk.Toplevel(self.root)
        window.title(title)
        window.configure(bg=PANEL)
        window.transient(self.root)
        window.grab_set()
        window.resizable(False, False)
        path = self.tk.StringVar(value=self.app.get(key, ''))
        saved = self.app.setdefault('art_crops', {}).get(crop_key, {})
        result = {'value': None}
        zoom = self.tk.DoubleVar(value=float(saved.get('zoom', 1.0)))
        focus_x = self.tk.DoubleVar(value=float(saved.get('x', 0.5)))
        focus_y = self.tk.DoubleVar(value=float(saved.get('y', 0.5)))
        preview_w = self.px(560)
        preview_h = max(self.px(180), round(preview_w * size[1] / size[0]))
        if preview_h > self.px(390):
            preview_h = self.px(390)
            preview_w = round(preview_h * size[0] / size[1])
        shell = self.tk.Frame(window, bg=PANEL)
        shell.pack(padx=self.px(24), pady=self.px(22))
        self.tk.Label(shell, text=title, bg=PANEL, fg=TEXT, font=self.font_title).pack(anchor='w')
        self.tk.Label(shell, text=_('Click the picture or Choose image; Restore goes back to the original.'),
                      bg=PANEL, fg=MUTED, font=self.font_ui).pack(anchor='w', pady=(2, self.px(12)))
        canvas = self.tk.Canvas(shell, width=preview_w, height=preview_h, bg=BG, highlightthickness=2,
                                highlightbackground=GOLD_DIM, bd=0, cursor='hand2')
        canvas.pack()
        preview = {'photo': None}

        def redraw(*_args):
            canvas.delete('all')
            try:
                with Image.open(path.get()) as source:
                    crop = {'zoom': zoom.get(), 'x': focus_x.get(), 'y': focus_y.get()}
                    image = uncharted_art.crop_image(source.convert('RGB'), (preview_w, preview_h), crop)
                preview['photo'] = ImageTk.PhotoImage(image)
                canvas.create_image(0, 0, image=preview['photo'], anchor='nw')
            except (OSError, ValueError, self.tk.TclError):
                preview['photo'] = None
                canvas.create_text(preview_w // 2, preview_h // 2, text=_('Original picture'), fill=MUTED,
                                   font=self.font_ui_md)
            canvas.create_rectangle(1, 1, preview_w - 2, preview_h - 2, outline=GOLD, width=2)

        def choose():
            chosen = self.image_path(title, path.get())
            if chosen:
                path.set(chosen)

        controls = self.tk.Frame(shell, bg=PANEL)
        controls.pack(fill='x', pady=(self.px(14), 0))
        for row, (label, variable, start, end, step) in enumerate((
                (_('Zoom'), zoom, 1.0, 3.0, 0.01), (_('Horizontal position'), focus_x, 0.0, 1.0, 0.01),
                (_('Vertical position'), focus_y, 0.0, 1.0, 0.01))):
            self.tk.Label(controls, text=label, bg=PANEL, fg=TEXT, font=self.font_ui,
                          width=20, anchor='w').grid(row=row, column=0, sticky='w', pady=2)
            self.tk.Scale(controls, variable=variable, from_=start, to=end, resolution=step,
                          orient='horizontal', showvalue=False, length=self.px(380), bg=PANEL,
                          fg=TEXT, troughcolor=CARD_HI, activebackground=GOLD, highlightthickness=0,
                          command=redraw).grid(row=row, column=1, sticky='ew', pady=2)
        buttons = self.tk.Frame(shell, bg=PANEL)
        buttons.pack(fill='x', pady=(self.px(16), 0))
        self.pill(buttons, _('Choose image'), choose, background=PANEL).pack(side='left')

        def restore():
            path.set('')
            zoom.set(1.0)
            focus_x.set(0.5)
            focus_y.set(0.5)

        self.pill(buttons, _('Restore'), restore, kind='ghost', background=PANEL).pack(side='left', padx=(6, 0))
        self.pill(buttons, _('Cancel'), window.destroy, kind='ghost', background=PANEL).pack(side='right')

        def save():
            result['value'] = (path.get(), {'zoom': zoom.get(), 'x': focus_x.get(), 'y': focus_y.get()})
            window.destroy()

        self.pill(buttons, _('Save changes'), save, kind='primary', background=PANEL).pack(side='right', padx=(0, 6))
        canvas.bind('<Button-1>', lambda _event: choose())
        for variable in (path, zoom, focus_x, focus_y):
            variable.trace_add('write', redraw)
        redraw()
        window.bind('<Escape>', lambda _event: window.destroy())
        window.wait_window()
        return result['value']

    def edit_launcher_art(self, key, title, size, crop_key):
        """Change image: the player's own picture for a cover or the banner (kept where it is, not copied)."""
        result = self.edit_art(key, title, size, crop_key)
        if not result:
            return
        self.app[key], self.app.setdefault('art_crops', {})[crop_key] = result
        self.collect()
        self.configure_art()
        self.banner_key = self.banner_scaled = None
        self.card_images = {}
        self.refresh()

    def photo_collection_banner(self, width, height):
        try:
            from PIL import ImageTk
            import uncharted_art
            page = tuple(int(BG[i:i + 2], 16) for i in (1, 3, 5))
            scene = uncharted_art.collection_banner(width, height, page)
            return ImageTk.PhotoImage(scene) if scene is not None else None
        except Exception:
            return None

    def draw_banner(self):
        self.draw_hero()

    def photo_hero(self, key, width, height):
        """Blurred cover backdrop when the dump has no pic1.png."""
        try:
            from PIL import ImageTk
            import uncharted_art
            page = tuple(int(BG[i:i + 2], 16) for i in (1, 3, 5))
            scene = uncharted_art.hero_backdrop(key, width, height, page)
            return ImageTk.PhotoImage(scene) if scene is not None else None
        except Exception:
            return None

    def photo_banner(self, key, width, height):
        return self.photo_hero(key, width, height)

    def cover_art(self, game_dir, side):
        try:
            from PIL import Image, ImageTk
            import uncharted_art
            art = uncharted_art.cover(game_dir)
            return ImageTk.PhotoImage(art.resize((side, side), Image.LANCZOS)) if art else None
        except Exception:
            return None

    def scaled_art(self, path, width):
        """The art cut to the banner, darkened (the parchment is light) and faded into the page."""
        try:
            from PIL import Image, ImageTk
            height = int(self.hero['height'])
            with Image.open(path) as image:
                scaled = image.convert('RGB').resize((width, round(image.height * width / image.width)), Image.LANCZOS)
            top = max(0, (scaled.height - height) // 10)  # pic1's figure has its face near the top
            art = scaled.crop((0, top, width, top + height))
            art = Image.blend(art, Image.new('RGB', art.size, (6, 5, 4)), 0.55)
            page = tuple(int(BG[i:i + 2], 16) for i in (1, 3, 5))
            fade = Image.linear_gradient('L').resize(art.size)
            fade = fade.point(lambda v: 0 if v < 90 else min(255, (v - 90) * 255 // 130))
            return ImageTk.PhotoImage(Image.composite(Image.new('RGB', art.size, page), art, fade))
        except (ImportError, OSError):
            return None

    def build_game(self):
        f = self.page('game', _('Game'), _('Your own decrypted dump of the collection (CUSA02343, CUSA02320, …).'))
        self.folder(f, 'game_dir', _('Game folder'), _('The folder with eboot.bin'),
                    _('The folder with eboot.bin, big2-ps4_Shipping.elf and big3-ps4_Shipping.elf. '
                      'Leave it empty if every game has a folder of its own, below.'),
                    on_change=self.refresh)
        self.folder(f, 'user_dir', _('Saves folder'), _('Where saves and the shader cache are kept'),
                    _('Empty: the user folder next to the launcher.'))
        self.check(f, 'portable', _('Portable: keep the launcher settings in this folder'), _(PORTABLE_HINT))
        self.var('portable').trace_add('write', lambda *_a: self.set_portable(self.var('portable').get()))
        self.section(f, _('Separate game folders'))
        self.note(f, _('Only when the games are not in one collection folder (each dumped on its own). A game with '
                       'a folder here is taken from it; the others from the collection folder above.'), top=2)
        for key, _title, subtitle, _exe in GAMES:
            self.folder(f, f'game_dir_{key}', _(subtitle.split(' · ')[0]), _('The folder of this game'),
                        _('Empty: the collection folder.'), on_change=self.refresh)
        self.section(f, _('Console'))
        self.row(f, _('Language'), self.choice(f, 'language', LANGUAGES, 30),
                 _('The language the PS4 would be set to; the games use it for text and speech.'))
        name = self.ttk.Entry(f, textvariable=self.var('player_name'), width=30)
        self.row(f, _('Player name'), name, _('Shown where the game uses the PSN name.'))
        self.check(f, 'skip_intro', _('Skip intro logos'),
                   _('Starts at the title screen without the Sony, Naughty Dog and Bluepoint logos '
                     'and their music (they cannot be skipped with a button). All three games.'))
        self.check(f, 'unlock_chapters', _('Unlock all chapters'),
                   _('Every chapter in Chapter Select and the Brutal difficulty from the start, in all three '
                     'games. Saves and progress are not changed; turned off, the menus show your own progress. '
                     'By BilalReloaded.'))
        self.check(f, 'unlock_rewards', _('Unlock all rewards'),
                   _('Every reward in Bonuses (skins, weapons, render modes, art galleries, gameplay tweaks such as '
                     'Slow Motion or Mirror World) and every story cutscene, in all three games; rewards that '
                     'normally need the game finished first can be turned on right away. Nothing is written to the '
                     'save: turned off, the menus show your own progress again. Treasures, medals and trophies are '
                     'not touched. By BilalReloaded.'))
        self.check(f, 'skip_friends_notice', _("Skip the friends' stats notice"),
                   _("Continue and New Game go straight into the game: the game gets a local player name "
                     "instead of 'signed out' and does not stop on the notice that friends' stats are not "
                     "available. Nothing goes online."))

    def build_display(self):
        f = self.page('display', _('Display'), _('How the game window is shown.'))
        self.check(f, 'fullscreen', _('Fullscreen'), _('Alt+Enter switches between window and fullscreen while you play.'))
        self.check(f, 'pause_in_background', _('Pause in the background'),
                   _('The game stands still while its window is minimized or another window has the focus, and its '
                     'pause menu opens when you come back to gameplay.'))
        self.row(f, _('Presentation'), self.choice(f, 'present_mode', PRESENT_MODES, 34),
                 _('Mailbox shows each frame as soon as it is ready without tearing; FIFO waits for the display.'))
        refresh = display_refresh_rate()
        hint = _('Gameplay keeps its normal speed at every setting; the cutscene videos always play at '
                 '60 FPS. Above your display\'s refresh rate use Mailbox or Immediate.')
        if refresh:
            hint += ' ' + _('Your display runs at {} Hz.').format(refresh)
        self.row(f, _('Frame rate'), self.choice(f, 'frame_rate', FRAME_RATES, 34), hint)
        self.row(f, _('Frame rate limit'), self.choice(f, 'fps_limit', FPS_LIMITS, 34),
                 _('Shows at most this many frames per second, generated frames included. It only '
                   'lowers the rate: a limit at or above what the frame rate (and frame generation) give '
                   'changes nothing. Lower it for a steadier picture on a slower PC or to save power.'))
        self.row(f, _('Frame pacing'), self.choice(f, 'frame_pacing', FRAME_PACING, 34),
                 _("Shows frames at even intervals for smoother motion. Off: as before. Balanced: a small extra "
                   "GPU load. Smoothest: the smoothest, but keeps the GPU busy and uses more power; best on "
                   "desktop PCs."))
        self.row(f, _('Idle game threads'), self.choice(f, 'job_threads', JOB_THREADS, 34),
                 _('Spinning keeps several CPU cores busy while the game waits for work. Sleeping saves them '
                   '(best for 4-8 thread CPUs) and is just as smooth.'))
        self.section(f, _('Resolution'))
        self.row(f, _('Window size'), self.choice(f, 'window_size', WINDOW_SIZES, 34),
                 _('The window\'s size in windowed mode. With Render resolution at 1920 × 1080, a larger '
                   'window, or fullscreen on a 1440p or 4K screen, gets the image upscaled to fit.'))
        self.row(f, _('Render resolution'), self.choice(f, 'render_resolution', RENDER_RESOLUTIONS, 58),
                 _('The size the game draws its picture at. Native draws at your window size (or your '
                   'screen in fullscreen) for the sharpest image. Quality, Balanced and Performance draw '
                   'a smaller picture and let DLSS or FSR rebuild it at full size, while the HUD stays '
                   'sharp. These go up to about 3440 × 1440; the 4K choices draw at 3840 × 2160 or 3840 × 1600 '
                   'whatever the window (experimental: only short tests so far). In busy '
                   'scenes the CPU usually sets the frame rate, so a smaller size gains little there.'))
        # Small cards: a note when the chosen size is above 1920 x 1080 (filled in once the card
        # is known and whenever the size, window or fullscreen changes).
        parent = self.target(f)
        self.vram_note = self.ttk.Label(parent, text='', style='Warn.TLabel', wraplength=self.px(560),
                                        justify='left')
        self.vram_note.grid(row=self.next_row(parent), column=1, sticky='w', pady=(0, 4))
        for key in ('render_resolution', 'window_size', 'fullscreen'):
            self.var(key).trace_add('write', lambda *_a: self.update_vram_note())
        self.row(f, _('Wide screens'), self.choice(f, 'ultrawide', ULTRAWIDE, 34),
                 _('On a screen or window wider than 16:9 (21:9, 32:9) gameplay shows more at the sides '
                   'and fills it. Menus, loading screens and cutscene videos stay 16:9; the in-game HUD '
                   'is stretched for now. Always fill never switches to 16:9 (if the switching '
                   'misfires for you): menus, loading screens and videos are then stretched.'))
        self.row(f, _('Field of view'), self.choice(f, 'fov', FIELDS_OF_VIEW, 34),
                 _('Shows more of the scene in every direction, also when aiming and in cutscenes.'))
        self.row(f, _('Texture filtering'), self.choice(f, 'anisotropy', ANISOTROPY, 34),
                 _('Anisotropic filtering: floors, roads and walls seen at a shallow angle stay sharp '
                   'into the distance. In Uncharted 3 with DLSS it makes the picture softer, so it is off by default.'))
        self.check(f, 'full_res_textures', _('Full resolution textures'),
                   _('Uncharted 2 and 3: every texture at its full size from the start, as the developers’ '
                     'debug switch does, instead of the smaller versions the games stream in first: trees, leaves and far '
                     'buildings stay sharp. Uses more video memory; about 1% slower. From BilalReloaded’s port.'))
        self.check(f, 'hires_shadows', _('Hi-res lamp and flashlight shadows'),
                   _('Uncharted 2 and 3: a bigger shadow map for lamps and the flashlight, sharper shadow '
                     'edges (the developers’ debug switch). From BilalReloaded’s port.'))
        self.check(f, 'bloom', _('Bloom'),
                   _('The soft glow around bright lights and glints (all three games share the effect); '
                     'turn it off for a crisper, flatter picture.'))
        self.row(f, _('Depth of field'), self.choice(f, 'depth_of_field', DEPTH_OF_FIELD, 34),
                 _('The blur of what is out of focus. Uncharted 2 and 3 blur the background while aiming and '
                   'in some scenes and cutscenes. "Off only while aiming" keeps the picture sharp while aiming, '
                   'the rest as the games have it; "Off everywhere" removes it in cutscenes too. '
                   'Uncharted 2 and 3 only.'))
        self.row(f, _('Upscaling'), self.choice(f, 'scaler', SCALERS, 34),
                 _('FSR 1 keeps edges sharp when the image is enlarged; bilinear is softer.'))
        self.row(f, _('Sharpening'), self.choice(f, 'sharpening', SHARPENING, 34),
                 _('AMD RCAS on the final picture (after FSR 1 when the image is enlarged, also at native size).'))
        self.section(f, _('HDR (experimental)'))
        self.row(f, _('HDR'), self.choice(f, 'hdr', HDR_MODES, 34),
                 _("HDR10 output on an HDR display (turn on 'Use HDR' in the Windows display settings first; without it the game starts in SDR). The games also get the brighter highlights of their own scenes with a render resolution and DLSS, FSR 4, XeSS or TAA on a GeForce; otherwise the game's picture is shown in HDR10 at the paper white below."))
        self.row(f, _('HDR tone mapping'), self.choice(f, 'hdr_tone', HDR_TONE_MODES, 34),
                 _("How the brighter highlights of the games' own scenes (see HDR above) are made. SDR + highlights: the game's SDR "
                   "picture as it is; only what is brighter than a knee gets the scene's own brightness. Game curve extended: the "
                   "game's own film curve up to its turning point, then straight on into HDR. RenoDX tone mapping: the RenoDX tone "
                   "mapper with its own picture settings. It can also be changed while playing (in-game menu, Insert key, HDR "
                   "section); the game starts with the choice made here."))
        self.row(f, _('Peak brightness (nits)'), self.choice(f, 'hdr_peak_nits', HDR_PEAK_NITS, 34),
                 _("The brightest highlights: set it to your display's peak brightness."))
        self.row(f, _('Paper white (nits)'), self.choice(f, 'hdr_paper_white', HDR_PAPER_WHITE, 34),
                 _('The brightness of white in the game, the HUD and the menus. 203 is the HDR reference; raise it in a bright room.'))
        self.section(f, _('Anti-aliasing'))
        self.aa_box = self.row(f, _('Anti-aliasing'), self.choice(f, 'anti_aliasing', self.aa_modes(), 34),
                 _('Temporal anti-aliasing with the games\' own motion vectors: smoother edges and less '
                   'shimmering than the games\' own. At Native render resolution DLSS is DLAA; with '
                   'Quality, Balanced or Performance DLSS or FSR also rebuilds the full size. DLSS needs '
                   'a GeForce RTX; FSR 4 runs best on a Radeon RX 7000/9000 or a GeForce RTX 40/50. The '
                   'first time FSR 4 starts (and after a graphics driver update) the game can pause for '
                   'about 10 seconds while the driver prepares it. Intel XeSS runs on any GPU (best on '
                   'Intel Arc).'))
        # FSR 4 v07 is not part of the package: the player can download it (fsr4_opt.py).
        card = self.target(f)
        r = self.next_row(card)
        self.ttk.Label(card, text=_('AMD FSR 4 files')).grid(row=r, column=0, sticky='nw', padx=(0, 24), pady=(9, 0))
        holder = self.tk.Frame(card, bg=CARD)
        holder.grid(row=r, column=1, sticky='w', pady=(5, 0))
        self.fsr4_button = self.pill(holder, _('Download FSR 4 files'), self.download_fsr4)
        self.fsr4_button.pack(side='left')
        self.fsr4_status = self.tk.Label(holder, text='', bg=CARD, fg=TEXT, font=self.font_ui, justify='left')
        self.fsr4_status.pack(side='left', padx=(12, 0))
        self.ttk.Label(card, text=_(
            'FSR 4 is not included with the port. The button downloads the FSR 4 files published by FireBurn/Q2RTX '
            "on GitHub (about 33 MB) into the fsr4_shaders folder, then builds the port's faster versions of two "
            'of their passes on this PC. The files come from that third party, not from this port, and AMD has '
            'not licensed them for this use: download them only if you are fine with that. Nothing is downloaded '
            'unless you press the button.'), style='Muted.TLabel', wraplength=self.px(560), justify='left').grid(
            row=r + 1, column=1, sticky='w', pady=(3, 4))
        self.fsr4_busy = False
        self.refresh_fsr4()
        self.row(f, _('DLSS model'), self.choice(f, 'dlss_model', DLSS_MODELS, 34),
                 _('With NVIDIA DLSS: the AI model it uses. K is NVIDIA\'s choice for this (DLAA) mode; '
                   'L and M are the newer DLSS 4.5 models, made for heavier upscaling. Compare them '
                   'in motion.'))
        self.row(f, _('Anti-aliasing sharpness'), self.choice(f, 'aa_sharpness', AA_SHARPNESS, 34),
                 _('Sharpens the picture after the anti-aliasing (DLSS, FSR, TAA), which can look '
                   'slightly soft, most at the upscaled render resolutions.'))
        self.section(f, _('Frame generation'))
        hint = _('Frames made between two of the game\'s, for smoother motion: 60 FPS shows as 120. '
                 'Use Mailbox or Immediate presentation and a display of at least that refresh rate. '
                 'Adds a little input latency; menus and cutscene videos are not generated. Use it '
                 'with the 60 or 120 FPS frame rate: above about 165 FPS it pauses.')
        if refresh:
            hint += ' ' + _('Your display runs at {} Hz.').format(refresh)
        self.row(f, _('Frame generation'), self.choice(f, 'frame_generation', FRAME_GENERATION, 34), hint)
        self.row(f, _('Multiplier'), self.choice(f, 'frame_gen_multiplier', FRAME_GEN_MULTIPLIERS, 34),
                 _('FSR 3 makes one frame (2×). DLSS makes up to three on a GeForce RTX 50 (4×), '
                   'one on an RTX 40.'))
        self.row(f, _('Low latency'), self.choice(f, 'low_latency', LOW_LATENCY, 34),
                 _('NVIDIA Reflex on GeForce cards, AMD Anti-Lag 2 on Radeon cards (with a recent driver). '
                   'The game waits to read the controller until the GPU is ready for the next frame, so '
                   'what you press shows sooner. It helps most when the GPU is the limit; the FPS '
                   'counter shows it when it is on.'))
        self.check(f, 'show_fps', _('Show the FPS counter'))
        self.section(f, _('Audio'))
        self.row(f, _('Speakers'), self.choice(f, 'speakers', SPEAKERS, 34),
                 _('What the games are told the speakers are, which sets the choices in their Audio Output '
                   'menu (stereo, 5.1, 7.1). Automatic follows the Windows speaker configuration of the '
                   'default playback device; set it to 5.1 or 7.1 when the games only offer stereo.'))

    def build_controls(self):
        f = self.page('controls', _('Controls'),
                      _('Keyboard, mouse and controller. They work together; changes apply when the game starts.'))
        self.key_buttons = {}
        self.capturing = None
        self.section(f, _('Mouse'), top=6)
        self.check(f, 'mouse_look', _('Look around with the mouse'),
                   _('The cursor stays in the game window; hold Left Alt (or switch windows) to free it. '
                     'Insert opens the in-game settings menu.'))
        from uncharted_widgets import Slider
        self.row(f, _('Mouse sensitivity'),
                 Slider(self.tk, self.target(f), self.var('mouse_sensitivity'), MOUSE_SENS_MIN, MOUSE_SENS_MAX, 0.05,
                        COLORS, self.px, self.font_ui_md, width=300, log=True, entry=True),
                 _('How far the camera turns for a mouse movement; 1.00 is the default. Drag the slider or '
                   'type a number (0.1 to 10) for a faster turn.'))
        self.row(f, _('Aiming sensitivity'),
                 Slider(self.tk, self.target(f), self.var('mouse_aim_sensitivity'), MOUSE_RATIO_MIN, MOUSE_RATIO_MAX,
                        0.05, COLORS, self.px, self.font_ui_md, width=300, log=True, entry=True),
                 _('Times the mouse sensitivity while aiming (right mouse button or L2): 1.00 turns as fast '
                   'as looking around, 0.50 half as fast for finer aim. The in-game menu (Insert) changes it '
                   'while you play.'))
        self.row(f, _('Vertical sensitivity'),
                 Slider(self.tk, self.target(f), self.var('mouse_vertical'), MOUSE_RATIO_MIN, MOUSE_RATIO_MAX,
                        0.05, COLORS, self.px, self.font_ui_md, width=300, log=True, entry=True),
                 _('Times the sensitivity for moving the mouse up and down, looking and aiming: 1.00 is the '
                   'same as sideways.'))
        self.row(f, _('Camera up and down'),
                 Slider(self.tk, self.target(f), self.var('mouse_camera_vertical'), MOUSE_RATIO_MIN, CAMERA_VERTICAL_MAX,
                        0.05, COLORS, self.px, self.font_ui_md, width=300, log=True, entry=True),
                 _('Times the up and down speed of the normal camera only (not while aiming). That camera moves '
                   'up and down along the game’s own arc, as with the right stick, which is slower than turning '
                   'sideways; 2 to 4 brings it close. The in-game menu (Insert) changes it while you play.'))
        self.check(f, 'mouse_invert_y', _('Invert the mouse vertically'))

        self.section(f, _('Button pictures'))
        self.row(f, _('Show in the game'), self.choice(f, 'button_prompts', BUTTON_PROMPTS, 40),
                 _('The pictures of buttons in menus and hints. Automatic shows your keys while you use '
                   'the keyboard or mouse, Xbox buttons after an Xbox (or other) controller, and the '
                   'PlayStation buttons after a PlayStation controller.'))
        self.row(f, _('Xbox button style'), self.choice(f, 'xbox_style', XBOX_STYLES, 40),
                 _("How the Xbox buttons look: antikmewzik's hand-drawn pictures with coloured halos, or "
                   "BilalReloaded's dark buttons with coloured letters and a softer glow. Takes effect at the next start."))

        self.section(f, _('Keyboard and mouse buttons'))
        self.note(f, _('Click a box, then press a key or a mouse button (the wheel counts too). '
                       '✕ removes a binding. Each action can have two.'), top=2)
        card = self.target(f)
        table = self.tk.Frame(card, bg=CARD)
        table.grid(row=self.next_row(card), column=0, columnspan=2, sticky='w', pady=(10, 0))
        rows = [(a, t) for a, t, _k, _p in BUTTON_ACTIONS] + [(a, t) for a, t, _k in STICK_ACTIONS]
        for r, (action, title) in enumerate(rows):
            self.ttk.Label(table, text=_(title)).grid(row=r, column=0, sticky='w', padx=(0, 24), pady=3)
            for slot in (0, 1):
                button = self.tk.Button(table, width=16, relief='flat', bg=CARD_HI, fg=TEXT,
                                        activebackground='#30251a', activeforeground=TEXT,
                                        font=self.font_ui, cursor='hand2', bd=0, padx=8, pady=4,
                                        command=lambda a=action, i=slot: self.capture_key(a, i))
                button.grid(row=r, column=1 + slot * 2, sticky='w', pady=3)
                clear = self.tk.Label(table, text='\ue711' if self.font_icon else '✕', bg=CARD, fg=MUTED,
                                      font=self.font_icon_sm or self.font_ui, cursor='hand2', padx=8)
                clear.grid(row=r, column=2 + slot * 2, sticky='w', padx=(2, 10))
                clear.bind('<Button-1>', lambda _e, a=action, i=slot: self.set_key(a, i, ''))
                clear.bind('<Enter>', lambda _e, w=clear: w.configure(fg=TEXT))
                clear.bind('<Leave>', lambda _e, w=clear: w.configure(fg=MUTED))
                self.key_buttons[(action, slot)] = button
        self.show_keys()
        holder = self.tk.Frame(card, bg=CARD)
        holder.grid(row=self.next_row(card), column=0, columnspan=2, sticky='w', pady=(12, 0))
        self.pill(holder, _('Default keys'), self.reset_keys).pack(side='left')
        self.row(f, _('Walk speed'),
                 Slider(self.tk, self.target(f), self.var('walk_speed'), WALK_SPEED_MIN, WALK_SPEED_MAX, 5,
                        COLORS, self.px, self.font_ui_md, fmt='{:.0f}', width=300, suffix='%'),
                 _('How far the walk key pushes the stick. Default is 65%; below about 55% the games do not move the character.'))

        self.section(f, _('Controller'))
        self.note(f, _('Which controller button gives each PS4 button (Xbox / PlayStation names).'), top=2)
        self.pad_vars = {}
        for action, title, _k, _p in BUTTON_ACTIONS:
            var = self.tk.StringVar(value=(self.app.get('pad_bindings') or {}).get(action, DEFAULT_PAD[action]))
            self.pad_vars[action] = var
            values = [v for v, _t in PAD_BUTTONS]
            box = self.ttk.Combobox(f, values=[_(t) for _v, t in PAD_BUTTONS], state='readonly', width=26)
            box.current(values.index(var.get()) if var.get() in values else values.index('none'))
            box.bind('<<ComboboxSelected>>', lambda _e, b=box, v=var: v.set(values[b.current()]))
            var.trace_add('write', lambda *_a, b=box, v=var: b.current(values.index(v.get())))
            self.row(f, _(title), box)
        self.check(f, 'pad_invert_y', _('Invert the right stick vertically'))
        self.check(f, 'pad_background', _('Keep the controller working in the background'),
                   _('The game takes the controller also while another window is in front (the game on one '
                     'screen, other programs on the other). The keyboard and mouse only work while the game '
                     'window is in front.'))
        card = self.target(f)
        holder = self.tk.Frame(card, bg=CARD)
        holder.grid(row=self.next_row(card), column=0, columnspan=2, sticky='w', pady=(12, 0))
        self.pill(holder, _('Default controller buttons'), self.reset_pad).pack(side='left')

        self.section(f, _('Vibration'))
        self.controller_name = self.ttk.Label(self.target(f), text=_('Detecting…'))
        self.row(f, _('Controller'), self.controller_name)
        self.controller_status = self.ttk.Label(self.target(f), text=_('Checking vibration support…'),
                                                style='Muted.TLabel')
        self.row(f, _('Status'), self.controller_status)
        self.check(f, 'pad_vibration', _('Controller vibration'),
                   _('The setting applies the next time the game starts.'))
        self.row(f, _('Vibration intensity'),
                 Slider(self.tk, self.target(f), self.var('pad_vibration_strength'), 0, 100, 5,
                        COLORS, self.px, self.font_ui_md, fmt='{:.0f}', width=300, suffix='%'))
        card = self.target(f)
        holder = self.tk.Frame(card, bg=CARD)
        holder.grid(row=self.next_row(card), column=0, columnspan=2, sticky='w', pady=(12, 0))
        self.controller_refresh_button = self.pill(holder, _('Detect controller'), self.detect_controller)
        self.controller_refresh_button.pack(side='left')
        self.controller_test_button = self.pill(holder, _('Test vibration'),
                                                lambda: self.detect_controller(test=True))
        self.controller_test_button.pack(side='left', padx=(8, 0))
        self.controller_test_button.configure(state='disabled')

    def key_list(self, action):
        value = key_bindings(self.app).get(action, '')
        parts = (value.split('|') + ['', ''])[:2]
        return [p.strip() for p in parts]

    def show_keys(self):
        for (action, slot), button in self.key_buttons.items():
            if self.capturing == (action, slot):
                button.configure(text=_('Press a key…'), bg=GOLD, fg='#141008')
            else:
                name = self.key_list(action)[slot]
                button.configure(text=name or '—', bg=CARD_HI, fg=TEXT if name else MUTED)

    def set_key(self, action, slot, name):
        keys = self.key_list(action)
        keys[slot] = name
        if name:  # one binding per key: taken from the action that had it
            bindings = dict(self.app.get('key_bindings') or {})
            for other in DEFAULT_KEYS:
                if other == action:
                    continue
                others = self.key_list(other)
                if name in others:
                    others = ['' if k == name else k for k in others]
                    bindings[other] = '|'.join(others).strip('|') if any(others) else ''
            self.app['key_bindings'] = bindings
        if not keys[0] and keys[1]:
            keys = [keys[1], '']
        bindings = dict(self.app.get('key_bindings') or {})
        bindings[action] = '|'.join(k for k in keys if k)
        self.app['key_bindings'] = bindings
        self.capturing = None
        self.release_capture()
        self.show_keys()

    def capture_key(self, action, slot):
        self.release_capture()
        self.capturing = (action, slot)
        self.show_keys()
        # The click that opened the capture must not be taken as the binding.
        self.root.after(200, self.arm_capture)

    def arm_capture(self):
        if not self.capturing:
            return
        self.root.bind_all('<KeyPress>', self.captured_key)
        self.root.bind_all('<ButtonPress>', self.captured_button)
        self.poll_side_buttons(side_buttons())

    def poll_side_buttons(self, held):
        """Tk on Windows never reports the side buttons (XBUTTON1/2: no <Button-4/5> event), so they
        are read from the system while a binding is being captured. `held`: their state a moment ago."""
        if not self.capturing:
            return
        down = side_buttons()
        if self.root.focus_displayof():  # only while the launcher is the active window
            for index, name in enumerate(('Mouse 4', 'Mouse 5')):
                if down[index] and not held[index]:
                    self.set_key(*self.capturing, name)
                    return
        self.side_poll = self.root.after(15, self.poll_side_buttons, down)

    def release_capture(self):
        for sequence in ('<KeyPress>', '<ButtonPress>'):
            self.root.unbind_all(sequence)
        if getattr(self, 'side_poll', None):
            self.root.after_cancel(self.side_poll)
            self.side_poll = None

    def captured_key(self, event):
        sym = event.keysym
        if sym in RESERVED_KEYS:
            self.status.configure(text=_(RESERVED_KEYS[sym]), fg=GOLD)
            return 'break'
        name = WIN_NUMPAD.get(event.keycode) if sys.platform == 'win32' else None
        name = name or TK_KEYS.get(sym)
        if not name and len(sym) == 1 and sym.isalnum():
            name = sym.upper()
        if not name and sym.startswith('F') and sym[1:].isdigit():
            name = sym
        if not name and sym.startswith('KP_') and sym[3:].isdigit():
            name = f'Keypad {sym[3:]}'
        if name:
            self.set_key(*self.capturing, name)
        return 'break'

    def captured_button(self, event):
        # 4 and 5 as other toolkits number the side buttons; 8 and 9 as X11 does
        name = {1: 'Mouse Left', 2: 'Mouse Middle', 3: 'Mouse Right', 4: 'Mouse 4', 5: 'Mouse 5',
                8: 'Mouse 4', 9: 'Mouse 5'}.get(event.num)
        if name:
            self.set_key(*self.capturing, name)
        return 'break'

    def captured_wheel(self, event):
        self.set_key(*self.capturing, 'Wheel Up' if event.delta > 0 else 'Wheel Down')
        return 'break'

    def show_imported_controls(self):
        """After game_environment took the in-game menu's controls in (import_game_controls): the Controls page shows
        them, so a later collect() keeps them."""
        for key in ('mouse_look', 'mouse_invert_y') + GAME_CONTROLS_VALUES:
            if key in self.vars:
                self.vars[key].set(self.app[key])
        if getattr(self, 'key_buttons', None):
            self.show_keys()

    def reset_keys(self):
        self.app['key_bindings'] = {}
        self.capturing = None
        self.release_capture()
        self.show_keys()

    def reset_pad(self):
        for action, var in self.pad_vars.items():
            var.set(DEFAULT_PAD[action])

    # ---- trophies ----------------------------------------------------------------------------
    def build_trophies(self):
        tk = self.tk
        f = self.page('trophies', _('Trophies'),
                      _("The games' own trophy lists. A trophy shows here, and as a notice in the game, "
                        'when you earn it.'))
        self.trophy_game = self.app.get('trophy_game') if self.app.get('trophy_game') in dict(
            (k, 1) for k, _t, _s, _e in GAMES) else GAMES[0][0]
        self.trophy_filter = 'all'
        self.trophy_icons, self.trophy_state, self.trophy_info = {}, None, {}
        bar = tk.Frame(f, bg=SURFACE)
        bar.grid(row=self.next_row(f), column=0, columnspan=2, sticky='we', pady=(0, self.px(10)))
        self.trophy_tabs = {}
        for key, title, _sub, _exe in GAMES:
            button = self.pill(bar, title, lambda k=key: self.set_trophy_game(k), background=SURFACE,
                               padding=(16, 7))
            button.pack(side='left', padx=(0, 8))
            self.trophy_tabs[key] = button
        self.trophy_filters = {}
        for name, title in (('locked', _('Locked')), ('earned', _('Earned')), ('all', _('All'))):
            button = self.pill(bar, title, lambda n=name: self.set_trophy_filter(n), background=SURFACE,
                               padding=(14, 7))
            button.pack(side='right', padx=(8, 0))
            self.trophy_filters[name] = button
        self.trophy_summary = tk.Label(f, text='', bg=SURFACE, fg=MUTED, font=self.font_ui, anchor='w',
                                       justify='left')
        self.trophy_summary.grid(row=self.next_row(f), column=0, columnspan=2, sticky='we', pady=(0, self.px(10)))
        self.trophy_body = tk.Frame(f, bg=SURFACE)
        self.trophy_body.grid(row=self.next_row(f), column=0, columnspan=2, sticky='we')
        self.root.after(1500, self.poll_trophies)

    def trophy_user_dir(self):
        return Path(self.var('user_dir').get().strip() or DATA_DIR / 'user')

    def trophy_info_for(self, key):
        """The trophy list of one game (texts in the console language), made from its trophy pack in
        the game folder when it is not there yet; None without a pack."""
        s = self.current()
        folder = game_folder(s, key)
        if not folder:
            return None
        stem = next(exe for k, _t, _s, exe in GAMES if k == key)
        out = DATA_DIR / 'out' / Path(stem).stem.split('-')[0] / 'trophy'
        own = bool(str(s.get(f'game_dir_{key}') or '').strip())
        language = str(s.get('language') or '1')
        try:
            trophies.prepare(folder, key, out, int(language) if language.isdigit() else 1, own)
        except Exception:  # a broken pack must not break the launcher
            return None
        info = trophies.read_cache(out)
        if info:
            info['folder'] = out
        return info

    def trophy_icon(self, info, number, size, earned):
        """The trophy's icon from the cache folder (dimmed while it is locked)."""
        name = (info['npcommid'], number, size, earned)
        if name not in self.trophy_icons:
            image = None
            try:
                from PIL import Image, ImageEnhance, ImageTk
                with Image.open(info['folder'] / f'TROP{number:03d}.PNG') as source:
                    icon = source.convert('RGBA').resize((size, size), Image.LANCZOS)
                if not earned:
                    icon = ImageEnhance.Brightness(ImageEnhance.Color(icon).enhance(0.15)).enhance(0.55)
                image = ImageTk.PhotoImage(icon)
            except (ImportError, OSError):
                pass
            self.trophy_icons[name] = image
        return self.trophy_icons[name]

    @staticmethod
    def mark_pill(button, on):
        kind = 'primary' if on else 'secondary'
        if button.kind != kind:
            button.kind = kind
            button.draw()

    def set_trophy_game(self, key):
        self.trophy_game, self.trophy_state = key, None
        self.app['trophy_game'] = key
        self.render_trophies()

    def set_trophy_filter(self, name):
        self.trophy_filter, self.trophy_state = name, None
        self.render_trophies()

    def render_trophies(self, force=False):
        """Redraws the list of the chosen game when what it shows changed."""
        key = self.trophy_game
        info = self.trophy_info.get(key)
        if info is None or force:
            info = self.trophy_info[key] = self.trophy_info_for(key) or False
        unlocked = trophies.read_unlocks(self.trophy_user_dir(), info['npcommid']) if info else {}
        state = (key, self.trophy_filter, tuple(sorted(unlocked.items())), bool(info))
        for k, button in self.trophy_tabs.items():
            self.mark_pill(button, k == key)
        for name, button in self.trophy_filters.items():
            self.mark_pill(button, name == self.trophy_filter)
        if state == self.trophy_state:
            return
        self.trophy_state = state
        tk = self.tk
        for child in self.trophy_body.winfo_children():
            child.destroy()
        if not info:
            self.trophy_summary.configure(text='')
            self.note_in(self.trophy_body, _('No trophy list for this game. Set the game folder on the Game page '
                                             '(it needs sce_sys/trophy/trophy0N.trp).'))
            return
        progress = trophies.progress(info, unlocked)
        grades = '   '.join(f"{_(trophies.GRADE_NAMES[g])} {progress['earned'][g]}/{progress['total'][g]}"
                            for g in 'PGSB' if progress['total'][g])
        self.trophy_summary.configure(
            text=info['title'] + '\n' + _('{} of {} earned · {}%').format(
                progress['count'], len(info['trophies']), progress['percent']) + f'   ·   {grades}')
        shown = 0
        size = self.px(52)
        for number, grade, hidden, name, detail in info['trophies']:
            when = unlocked.get(number)
            if (self.trophy_filter == 'earned') != (when is not None) and self.trophy_filter != 'all':
                continue
            shown += 1
            if hidden and when is None:
                name, detail = _('Hidden trophy'), _('Keep playing to find out how to earn it.')
            card = tk.Frame(self.trophy_body, bg=CARD, highlightthickness=1, highlightbackground=LINE)
            card.pack(fill='x', pady=(0, 6))
            icon = self.trophy_icon(info, number, size, when is not None)
            if icon:
                tk.Label(card, image=icon, bg=CARD).pack(side='left', padx=(10, 12), pady=8)
            side = tk.Frame(card, bg=CARD)
            side.pack(side='right', padx=(10, 16))
            color = TROPHY_COLORS.get(grade, MUTED) if when is not None else MUTED
            tk.Label(side, text=_(trophies.GRADE_NAMES.get(grade, 'Bronze')), bg=CARD, fg=color,
                     font=('Segoe UI Semibold', 9)).pack(anchor='e')
            tk.Label(side, text=time.strftime('%d %b %Y', time.localtime(when)) if when is not None else _('Locked'),
                     bg=CARD, fg=MUTED, font=self.font_ui_sm).pack(anchor='e', pady=(2, 0))
            middle = tk.Frame(card, bg=CARD)
            middle.pack(side='left', fill='x', expand=True, pady=8)
            tk.Label(middle, text=name, bg=CARD, fg=TEXT if when is not None else MUTED, anchor='w',
                     font=self.font_ui_md).pack(anchor='w')
            tk.Label(middle, text=detail, bg=CARD, fg=MUTED, font=self.font_ui_sm, justify='left', anchor='w',
                     wraplength=self.px(520)).pack(anchor='w', pady=(1, 0))
        if not shown:
            self.note_in(self.trophy_body, _('Nothing here yet.') if self.trophy_filter == 'earned'
                         else _('Every trophy is earned.'))

    def note_in(self, parent, text):
        self.tk.Label(parent, text=text, bg=SURFACE, fg=MUTED, font=self.font_ui, anchor='w', justify='left',
                      wraplength=self.px(640)).pack(anchor='w', pady=(8, 0))

    def poll_trophies(self):
        """Every 1.5 s while the page is open: the game writes an unlock as it happens."""
        if self.current_page == 'trophies':
            try:
                self.render_trophies()
            except Exception:
                pass
        self.root.after(1500, self.poll_trophies)

    def build_cheats(self):
        f = self.page('cheats', _('Cheats'), _('Each cheat only affects its own game; changes apply when the game starts.'))
        for game, title, subtitle, _exe in GAMES:
            self.section(f, f'{title} · {subtitle}')
            self.check(f, f'{game}_infinite_health', _('Infinite Health'))
            self.check(f, f'{game}_infinite_ammo', _('Infinite Ammo'))
            for cheat in SWITCH_CHEATS.get(game, ()):
                self.check(f, f'{game}_{cheat}', _(SWITCH_CHEAT_TITLES[cheat]))
            self.check(f, f'{game}_devmode', _("Developer mode: Naughty Dog's debug text; F11 Dev Menu, F8 Quick Menu"))
            self.check(f, f'{game}_devmode_text', _('Show the debug text (FPS, timings, memory) in developer mode; F7 in play'), indent=40)

    def build_advanced(self):
        f = self.page('advanced', _('Advanced'), _('Maintenance and options for testing.'))
        self.section(f, _('Launcher language'), top=2)
        self.row(f, _('Interface language'),
                 self.choice(f, 'ui_language', uncharted_i18n.LANGUAGES, 24, self.change_ui_language),
                 _('This changes only the launcher. The game language is selected on the Game page.'))
        self.check(f, 'close_on_play', _('Close the launcher when the game starts'))
        extra = self.ttk.Entry(f, textvariable=self.var('extra_env'), width=60)
        self.row(f, _('Environment'), extra, _('Extra variables for the game, NAME=value separated by spaces.'))
        self.section(f, _('Shader preparation'))
        self.note(f, _("Each game's shaders are built once for your graphics card, so play does not stutter "
                       'when it meets a new one. It happens on the first start, or here ahead of time.'), top=2)
        self.row(f, _('When'), self.choice(self.target(f), 'shader_preparation', [
            ('before', 'Before playing (recommended)'), ('background', 'In the background while playing'),
            ('off', 'Off (shaders are built as the game meets them)')]),
            _('In the background the game starts at once but stutters until it is done.'))
        self.row(f, _('Speed'), self.choice(self.target(f), 'preparation_speed', [
            ('all', 'All CPU cores (fastest)'), ('half', 'Half the cores (the PC stays responsive)')]))
        self.check(f, 'prep_unknown_versions', _('Also prepare versions the port has no shader index for'),
                   _("Other regions' releases of a known game version. The "
                     'first start then reads all the game files (about 60 GB) once to find the shaders. This can '
                     'take a very long time: leave it running, a progress bar shows how far it is.'))
        self.var('prep_unknown_versions').trace_add('write', lambda *_a: self.update_prep_status())
        card = self.target(f)
        self.prep_status = self.tk.Label(card, text='', bg=CARD, fg=TEXT, font=self.font_ui, justify='left')
        self.prep_status.grid(row=self.next_row(card), column=0, columnspan=2, sticky='w', pady=(12, 0))
        holder = self.tk.Frame(card, bg=CARD)
        holder.grid(row=self.next_row(card), column=0, columnspan=2, sticky='w', pady=(12, 0))
        self.pill(holder, _('Prepare shaders now'), lambda: self.play(prepare=True)).pack(side='left')
        self.pill(holder, _('Prepare again'), self.prepare_again).pack(side='left', padx=(8, 0))
        # Beside the buttons, filled from the game's PREP lines while it prepares (no game window then).
        self.prep_bar = self.ttk.Progressbar(holder, style='Gold.Horizontal.TProgressbar', maximum=100,
                                             length=self.px(360))
        self.var('start').trace_add('write', lambda *_a: self.update_prep_status())
        self.update_prep_status()
        self.section(f, _('Shader cache'))
        self.note(f, _('Compiled shaders are kept so the game does not stutter again. Clear the cache only '
                       'when the picture is broken after an update.'), top=2)
        card = self.target(f)
        holder = self.tk.Frame(card, bg=CARD)
        holder.grid(row=self.next_row(card), column=0, columnspan=2, sticky='w', pady=(12, 0))
        self.pill(holder, _('Clear shader cache'), self.clear_cache).pack(side='left')
        self.pill(holder, _('Open the saves folder'),
                  lambda: self.open_path(self.var('user_dir').get(), 'user_dir')).pack(side='left', padx=(8, 0))
        self.section(f, _('Updates'))
        if UPDATES:
            self.check(f, 'check_updates', _('Look for a new version when the launcher starts'))
        card = self.target(f)
        holder = self.tk.Frame(card, bg=CARD)
        holder.grid(row=self.next_row(card), column=0, columnspan=2, sticky='w', pady=(12, 0))
        if UPDATES:
            self.pill(holder, _('Check for updates'),
                      lambda: threading.Thread(target=self.check_update, args=(True,), daemon=True).start()
                      ).pack(side='left')
            self.pill(holder, _('Releases page'), lambda: webbrowser.open(RELEASES_PAGE)).pack(side='left', padx=(8, 0))
            self.pill(holder, _("What's new"), self.show_notes).pack(side='left', padx=(8, 0))
        else:
            self.tk.Label(holder, text=_('This is an unofficial fork (version {}): it does not update '
                                         "itself. Official releases are on SuperMedo's page; "
                                         'installing one replaces this build.').format(VERSION),
                          bg=CARD, fg=MUTED, font=self.font_ui_sm, wraplength=self.px(560),
                          justify='left').pack(anchor='w')
            self.pill(holder, _('Official releases'), lambda: webbrowser.open(RELEASES_PAGE)
                      ).pack(anchor='w', pady=(8, 0))
        if UPDATES:
            self.build_versions(f)
        self.section(f, _('Reset'))
        self.note(f, _('Puts every display, controls, cheats, game and advanced option back to its default. The '
                       'game folders, the saves folder, the console and launcher languages, the player name and '
                       'the cover art stay as they are. Your saves are not touched.'), top=2)
        card = self.target(f)
        holder = self.tk.Frame(card, bg=CARD)
        holder.grid(row=self.next_row(card), column=0, columnspan=2, sticky='w', pady=(12, 0))
        self.pill(holder, _('Reset all settings'), self.ask_reset).pack(side='left')
        self.reset_confirm = self.tk.Frame(holder, bg=CARD)
        self.tk.Label(self.reset_confirm, text=_('Reset every setting to its default?'), bg=CARD, fg=GOLD,
                      font=self.font_ui).pack(side='left')
        self.pill(self.reset_confirm, _('Reset'), self.reset_all).pack(side='left', padx=(12, 0))
        self.pill(self.reset_confirm, _('Cancel'), self.cancel_reset).pack(side='left', padx=(8, 0))

    def build_versions(self, f):
        """Advanced > Updates: the Version row (another release of the port, chosen from GitHub's list), the test
        builds switch, a line for what the switch does or how far it is, and a button per release kept in versions/."""
        card = self.target(f)
        holder = self.tk.Frame(card, bg=CARD)
        self.version_box = self.ttk.Combobox(holder, state='readonly', width=34, values=[])
        self.version_box.set(f"{VERSION} {_('(installed)')}")
        self.version_box.pack(side='left')
        self.version_box.bind('<<ComboboxSelected>>', lambda _e: self.version_selected())
        self.version_button = self.pill(holder, _('Change version'), self.change_to_selected, padding=(14, 6))
        self.version_button.pack(side='left', padx=(8, 0))
        self.row(f, _('Version'), holder,
                 _('Installed: {}. Another release of the port: go back to an earlier one if this one has a problem, '
                   'and forward again. Saves, settings and the shader cache stay as they are.').format(VERSION))
        self.check(f, 'show_test_builds', _('Show test builds (pre-releases)'))
        self.var('show_test_builds').trace_add('write', lambda *_a: self.show_versions())
        card = self.target(f)
        self.version_note = self.tk.Label(card, text='', bg=CARD, fg=GOLD, font=self.font_ui, justify='left',
                                          wraplength=self.px(640))
        self.version_note.grid(row=self.next_row(card), column=0, columnspan=2, sticky='w', pady=(8, 0))
        # Back to <version> for the kept release (not one of the installed version, which an update can leave),
        # and Remove backups for the whole versions/ folder; neither shows with versions/ empty (Remove backups also
        # shows for what is left there that is no whole backup).
        kept = version_backups()
        self.version_backup_buttons, self.remove_backups_button = [], None
        if kept or not folder_empty(VERSIONS_DIR):
            self.backups_holder = self.tk.Frame(card, bg=CARD)
            self.backups_holder.grid(row=self.next_row(card), column=0, columnspan=2, sticky='w', pady=(4, 0))
            installed = installed_release()
            for version, path in [(v, b) for v, b in kept
                                  if v not in (installed, backup_folder_name(installed))][:KEEP_BACKUPS]:
                button = self.pill(self.backups_holder, _('Back to {}').format(version),
                                   lambda v=version, b=path: self.change_version(v, backup=b), padding=(14, 6))
                button.pack(side='left', padx=(0, 8))
                button.backup = path
                self.version_backup_buttons.append(button)
            self.remove_backups_button = self.pill(self.backups_holder, _('Remove backups'), self.remove_backups,
                                                   kind='ghost', padding=(12, 6))
            self.remove_backups_button.pack(side='left')
            if self.backups_mb is not None:
                self.remove_backups_button.configure(text=_('Remove backups ({} MB)').format(self.backups_mb))
        self.version_list = []
        if self.releases is not None:
            self.show_versions()

    def show_backups_size(self):
        """Remove backups (<size>): versions/ measured once, the first time the Advanced page shows."""
        if not self.remove_backups_button:
            return
        if self.backups_mb is None:
            self.backups_mb = round(folder_size(VERSIONS_DIR) / (1 << 20))
        self.remove_backups_button.configure(text=_('Remove backups ({} MB)').format(self.backups_mb))

    def refuse_player_folders(self, roots):
        """Whether a player's folder (the saves folder, a game folder; as set in the window now) is in one of roots,
        said in a message: a version switch would move it into a backup, and a later prune or Remove backups would
        delete it."""
        settings = {**self.app, **{key: v.get() for key, v in self.vars.items()}}
        inside = folders_inside(settings, roots)
        if inside:
            self.messagebox.showinfo(NAME, _("This folder is inside the port's program files or its versions folder, "
                                             'which a version change moves and Remove backups deletes: {}. Move it '
                                             'somewhere else and choose it again in the launcher first.'
                                             ).format(inside[0]))
        return bool(inside)

    def remove_backups(self):
        """Deletes versions/ (the kept releases) after a confirmation that says the space it frees."""
        if self.updating or self.refuse_player_folders([VERSIONS_DIR]):
            return
        if self.backups_mb is None:
            self.backups_mb = round(folder_size(VERSIONS_DIR) / (1 << 20))
        if not self.messagebox.askyesno(NAME, _('Delete the kept versions? This frees {} MB. The installed version '
                                                'stays as it is; going back to another one then needs a '
                                                'download.').format(self.backups_mb)):
            return
        try:
            remove_backups(VERSIONS_DIR)
        except OSError as error:
            self.version_note.configure(text=_('Could not remove the backups: {}').format(error), fg='#e07a4f')
            self.refresh_backups()
            return
        self.backups_holder.grid_remove()
        self.version_backup_buttons, self.remove_backups_button = [], None
        self.version_note.configure(text=_('Backups removed: {} MB freed.').format(self.backups_mb), fg=MUTED)
        self.backups_mb = 0

    def refresh_backups(self):
        """The Back to buttons and the size on Remove backups from versions/ as it is now: after a Remove backups that
        failed partway, a backup it half deleted is no longer offered."""
        whole = {path for _v, path in version_backups()}
        for button in [b for b in self.version_backup_buttons if b.backup not in whole]:
            button.destroy()
            self.version_backup_buttons.remove(button)
        self.backups_mb = None
        self.show_backups_size()

    def load_releases(self):
        """Helper thread: GitHub's release list for Advanced > Version (once a session)."""
        try:
            releases = github_json(RELEASES_LIST_API)
            if not isinstance(releases, list):
                raise ValueError('unexpected answer from GitHub')
        except (OSError, ValueError) as failure:
            def failed(error=failure):
                self.releases_loading = False  # opening the page again tries again
                if self.version_note.winfo_exists():
                    self.version_note.configure(text=_('Could not load the list of versions: {}').format(error),
                                                fg=MUTED)
            self.ui_calls.put(failed)
            return
        self.releases = releases
        self.ui_calls.put(self.show_versions)

    def show_versions(self):
        """The Version drop-down filled from the release list (test builds with the switch on), the installed one
        selected."""
        if self.releases is None or not self.version_box.winfo_exists():
            return
        self.version_list = release_choices(self.releases, bool(self.var('show_test_builds').get()))
        labels, current = [], installed_release()
        for name, _url, _page, test in self.version_list:
            label = f"{name} {_('(installed)')}" if name == current else name
            labels.append(f"{label} {_('(test build)')}" if test else label)
        self.version_box.configure(values=labels)
        installed = [i for i, entry in enumerate(self.version_list) if entry[0] == current]
        if installed:
            self.version_box.current(installed[0])
        else:
            self.version_box.set(f"{current} {_('(installed)')}")
        self.version_selected()

    def version_selected(self):
        """The line under the Version row: what a switch to the chosen release means."""
        if self.updating:
            return
        index = self.version_box.current()
        current = installed_release()
        chosen = self.version_list[index][0] if 0 <= index < len(self.version_list) else current
        # The shader cache version of another release is not known here: the line says it may prepare again.
        self.version_note.configure(text='' if chosen == current else
                                    _('This version may prepare its shaders once more.'), fg=GOLD)

    def change_to_selected(self):
        index = self.version_box.current()
        if not 0 <= index < len(self.version_list) or self.version_list[index][0] == installed_release():
            self.version_note.configure(text=_('Choose another version in the list first.'), fg=MUTED)
            return
        name, url, page, _test = self.version_list[index]
        # A release kept in versions/ comes back from there, without a download: only one that says it is this
        # release (RELEASE_FILE; the backup of a test build has the number of the release without its suffix).
        backup = next((path for _v, path in version_backups() if release_name(path) == name), None)
        self.change_version(name, url, page, backup)

    def change_version(self, version, url=None, page=None, backup=None):
        """Changes the installed release to version: downloaded from url (as an update is), or moved back from
        backup (a release kept in versions/). A copy of this launcher in UPDATE_DIR does the switch once this window
        has closed (switch_version): this release goes into versions/<VERSION>, the other one in, and it starts."""
        if self.process:
            self.messagebox.showinfo(NAME, _('Close the game before changing the version.'))
            return
        if self.updating:
            return
        if not FROZEN:  # a source tree changes versions with git
            webbrowser.open(page or RELEASES_PAGE)
            return
        if self.refuse_player_folders([PORT_DIR / name for name in PROGRAM_FILES] + [PORT_DIR / 'versions']):
            return
        question = _('Change to version {}? The launcher closes, changes the version and opens again. Saves, '
                     'settings and the shader cache are kept. Version {} is kept in the versions folder, so you can '
                     'come back to it.').format(version, installed_release())
        question += '\n\n' + _('This version may prepare its shaders once more.')
        if version_tuple(version) < FIRST_SWITCH_VERSION:
            question += '\n\n' + _('Version {} has no version choice: its Update button brings you back to the newest '
                                    'version.').format(version)
        if not self.messagebox.askyesno(NAME, question):
            return
        self.updating = True
        # Update too: a second download would share UPDATE_DIR with this one.
        buttons = [self.version_button, *self.version_backup_buttons]
        if self.update_box and self.update_box.winfo_exists():
            buttons.append(self.update_button)
        for button in buttons:
            button.configure(state='disabled')

        def progress(percent):
            text = _('Downloading version {}… {}%').format(version, percent)
            if self.version_note.winfo_exists():
                self.version_note.configure(text=text, fg=GOLD)
            self.status.configure(text=text, fg=GOLD)

        def failed(error):
            self.updating = False
            for button in buttons:
                if button.winfo_exists():
                    button.configure(state='normal')
            if self.version_note.winfo_exists():
                self.version_note.configure(text=_('Could not change the version: {}').format(error), fg='#e07a4f')
            self.status.configure(text='', fg=MUTED)

        def start(helper, source):
            """On the window's thread: the game started during the download (Play stays usable) would hold the
            installation's files, and this launcher would stay open for it, so the switcher would wait in vain."""
            if self.process:
                failed(_('Close the game before changing the version.'))
                return
            try:
                subprocess.Popen([str(helper / 'Uncharted.exe'), '--switch-version', str(PORT_DIR), str(source),
                                  installed_release(), str(os.getpid())], cwd=str(helper), stdin=subprocess.DEVNULL,
                                 creationflags=NO_WINDOW)
            except OSError as error:
                failed(error)
                return
            self.close()

        def work():
            try:
                source = backup or download_release(url, lambda percent: self.ui_calls.put(lambda p=percent: progress(p)))
                # The switch runs from a copy of this launcher outside the installation: it moves this
                # installation's files, which a launcher running from them could not.
                helper = UPDATE_DIR / 'switcher'
                shutil.rmtree(helper, ignore_errors=True)
                helper.mkdir(parents=True)
                shutil.copy2(PORT_DIR / 'Uncharted.exe', helper / 'Uncharted.exe')
                if (PORT_DIR / '_internal').is_dir():
                    shutil.copytree(PORT_DIR / '_internal', helper / '_internal')
            except Exception as failure:  # every failure, not only an OSError: never a thread ending in silence
                shutil.rmtree(UPDATE_DIR, ignore_errors=True)  # the download and the switcher copy
                self.ui_calls.put(lambda error=failure: failed(error))
                return
            self.ui_calls.put(lambda: start(helper, source))
        threading.Thread(target=work, daemon=True).start()

    def build_log(self):
        tk = self.tk
        page = tk.Frame(self.content, bg=SURFACE)
        self.pages['log'] = page
        shell = tk.Frame(page, bg=SURFACE)
        shell.pack(fill='both', expand=True, padx=self.px(18), pady=(self.px(14), self.px(14)))
        head = tk.Frame(shell, bg=SURFACE)
        head.pack(fill='x', padx=self.px(26), pady=(self.px(18), self.px(14)))
        tk.Label(head, text=_('Log'), bg=SURFACE, fg=TEXT, font=self.font_title).pack(side='left')
        self.pill(head, _('Copy'), self.copy_log, background=SURFACE, padding=(16, 6)).pack(side='right')
        self.pill(head, _('Open log folder'), self.open_log_folder, background=SURFACE, padding=(16, 6)).pack(
            side='right', padx=(0, self.px(8)))
        body = tk.Frame(shell, bg=SURFACE)
        body.pack(fill='both', expand=True, padx=self.px(26), pady=(0, self.px(8)))
        self.log = tk.Text(body, bg=CARD, fg='#e3d6bd', insertbackground=TEXT, font=('Cascadia Mono', 9), wrap='none',
                           relief='flat', state='disabled', highlightbackground=LINE, highlightthickness=1,
                           padx=self.px(12), pady=self.px(10))
        bar = self.ttk.Scrollbar(body, orient='vertical', command=self.log.yview)
        self.log.configure(yscrollcommand=bar.set)
        bar.pack(side='right', fill='y')
        self.log.pack(fill='both', expand=True)

    def current(self):
        """The settings as they stand on screen (not yet saved)."""
        return {**self.app, **{key: v.get() for key, v in self.vars.items()}}

    def refresh(self):
        """Game cards, the ready line and the status for the current choices."""
        s = self.current()
        start = s['start']
        self.info = game_info(game_folder(s, start))
        info = self.info
        # Nothing set yet: no cover is dimmed.
        unset = not any(game_folder(s, k) for k, _t, _s, _e in GAMES)
        self.available = {key: unset or game_available(s, key) for key, _t, _s, _e in GAMES}
        for key in self.cards:
            self.draw_card(key)
        if not info:
            ready = _('Choose the game folder (the one with eboot.bin) on the Game page.')
        else:
            missing = [title for key, title, _s, _e in GAMES if not self.available[key]]
            ready = f"{info['title']} · {info['id']} · {_('version')} {info['version']}"
            if missing:
                ready += '\n' + _('Not in this folder: ') + ', '.join(missing)
            if unsupported_version(s, start):
                ready += '\n' + _('Shader preparation is off for this copy of the game (version {}): the port '
                                  'does not know where its shaders are yet.').format(info['version'])
                if known_code_without_index(s, start):
                    ready += ' ' + _('Advanced > Shader preparation can turn it on for this version.')
            elif needs_index_scan(s, start) and not shaders_prepared(s, start):
                ready += '\n' + _('No shader index ships for this copy ({}): the first shader preparation reads all '
                                  'the game files once. This can take a very long time: leave it running, a '
                                  'progress bar shows how far it is.').format(info['id'])
        self.ready.configure(text=ready)
        self.update_prep_status()
        if not self.process:
            title = next(t for k, t, _s, _e in GAMES if k == start)
            self.status.configure(text=_('Ready: {}').format(title) if info else _('No game folder yet.'),
                                  fg=MUTED)
        self.draw_hero()

    # ---- actions -----------------------------------------------------------------------------
    def change_ui_language(self):
        """Save and rebuild every label as soon as the interface language changes."""
        global UI_LANGUAGE
        page = self.current_page or 'advanced'
        self.save_settings()  # a locked settings file must not keep the old language on screen
        UI_LANGUAGE = uncharted_i18n.language_code(self.app['ui_language'])
        self.rebuild(page)

    def ask_reset(self):
        """Reset all settings, step one: the confirmation line beside the button."""
        self.reset_confirm.pack(side='left', padx=(self.px(16), 0))

    def cancel_reset(self):
        self.reset_confirm.pack_forget()

    def reset_all(self):
        """Reset all settings, step two: every option back to its default (reset_settings keeps the folders,
        the languages, the player name and the cover art), saved, and the pages built again."""
        new = reset_settings(self.current())
        self.app.clear()
        self.app.update(new)
        self.vars = {}
        self.pad_vars = None  # (collect must not write the old controller buttons back)
        self.save_settings()
        self.rebuild('advanced')
        self.status.configure(text=_('All settings are back to their defaults.'), fg=GOLD)

    def rebuild(self, page):
        """Every page built again from the settings (a new interface language, a reset)."""
        for child in self.root.winfo_children():
            child.destroy()
        # The rebuild drops the widgets these point at: a kept update_box hid the offer and made
        # "Check for updates" do nothing until a restart; the new hide box needs its trace again.
        self.vars = {}
        self.update_box = self.notes_window = None
        self.notes_hide_traced = False
        self.build()
        self.show(page)
        self.refresh()
        if self.update_offer:
            self.offer_update(*self.update_offer)
        if self.process:
            self.play_button.configure(state='disabled')
            self.stop_button.configure(state='normal')
            if self.preparing:
                self.prep_bar.pack(side='left', padx=(self.px(28), 0))
                self.prep_status.configure(text=_('Preparing shaders…'), fg=GOLD)
            self.status.configure(text=_('The game is running.') if self.entered
                                  else _('Preparing shaders…') if self.preparing
                                  else _('Preparing the game; it opens in its own window…'), fg=GOLD)
        self.detect_controller()

    def set_portable(self, on):
        """Moves the launcher's settings between %APPDATA% and launcher_settings.json next to the launcher."""
        global CONFIG_DIR, CONFIG_FILE
        if on == (CONFIG_FILE == PORTABLE_FILE):
            return
        appdata = Path(os.environ.get('APPDATA') or os.path.expanduser('~')) / 'uncharted-launcher'
        old_file = CONFIG_FILE
        CONFIG_DIR, CONFIG_FILE = (DATA_DIR, PORTABLE_FILE) if on else (appdata, appdata / 'settings.json')
        try:
            self.collect()   # writes the settings to the new place
            if not on:
                PORTABLE_FILE.unlink(missing_ok=True)
        except OSError as error:
            CONFIG_DIR, CONFIG_FILE = old_file.parent, old_file
            self.var('portable').set(not on)
            self.messagebox.showerror(NAME, _('Could not move the launcher settings: {}').format(error))

    def collect(self):
        for key, v in self.vars.items():
            self.app[key] = v.get()
        if getattr(self, 'pad_vars', None):
            self.app['pad_bindings'] = {a: v.get() for a, v in self.pad_vars.items()}
        CONFIG_DIR.mkdir(parents=True, exist_ok=True)
        write_atomic(CONFIG_FILE, json.dumps(self.app, indent=2) + '\n')
        try:
            set_ini('show_fps', '1' if self.app['show_fps'] else '0')
        except OSError:
            pass

    def detect_controller(self, test=False):
        """Detect or briefly rumble the first SDL gamepad without starting the game renderer."""
        if self.process:
            self.controller_status.configure(text=_('Unavailable while the game is running.'))
            return
        probe = controller_probe_path()
        if not probe:
            self.controller_name.configure(text=_('Not available'))
            self.controller_status.configure(text=_('bb-probe.exe was not found.'))
            return
        self.controller_refresh_button.configure(state='disabled')
        self.controller_test_button.configure(state='disabled')
        self.controller_status.configure(text=_('Testing…') if test else _('Detecting…'))
        strength = vibration_strength(self.current())

        def work():
            command = [str(probe), '--controller-test', str(strength)] if test \
                else [str(probe), '--controller-info']
            try:
                result = subprocess.run(command, capture_output=True, text=True, encoding='utf-8',
                                        errors='replace', timeout=5, env=controller_probe_environment(probe),
                                        creationflags=NO_WINDOW)
                if result.returncode:
                    raise OSError(result.stderr.strip() or _('controller probe failed'))
                info, error = parse_controller_probe(result.stdout), ''
            except (OSError, subprocess.TimeoutExpired) as failure:
                info, error = None, str(failure)
            self.ui_calls.put(lambda: self.show_controller(info, error, test))
        threading.Thread(target=work, daemon=True).start()

    def show_controller(self, info, error, test):
        if self.process:
            self.controller_refresh_button.configure(state='disabled')
            self.controller_test_button.configure(state='disabled')
            self.controller_status.configure(text=_('Unavailable while the game is running.'))
            return
        self.controller_refresh_button.configure(state='normal')
        if error:
            self.controller_name.configure(text=_('Not available'))
            self.controller_status.configure(text=_('Could not detect the controller: {}').format(error[:120]))
            return
        if not info['connected']:
            self.controller_name.configure(text=_('No controller detected'))
            self.controller_status.configure(text=_('Connect a controller, then detect again.'))
            return
        self.controller_name.configure(text=info['name'] or _('Game controller'))
        self.controller_test_button.configure(state='normal' if info['rumble'] else 'disabled')
        if not info['rumble']:
            status = _('Connected · Vibration not supported')
        elif test:
            status = _('Connected · Vibration test completed') if info['tested'] \
                else _('Connected · Vibration test failed')
        else:
            status = _('Connected · Vibration supported')
        self.controller_status.configure(text=status)

    def aa_modes(self):
        """The anti-aliasing choices: FSR 4 once its files are in fsr4_shaders (Display page download)."""
        modes = [m for m in ANTI_ALIASING if m[0] != 'fsr4' or not fsr4_opt.missing(FSR4_DIR)]
        return modes + ([FSR411] if fsr411_assets() else []) + ([XESS] if xess_available() else [])

    def refresh_fsr4(self, text=None):
        files, opt = fsr4_opt.missing(FSR4_DIR), fsr4_opt.opt_missing(FSR4_DIR)
        if text is None:
            text = (_('Installed') if not files and not opt else
                    _('Downloaded; the faster passes are not built yet') if not files else
                    _('Not installed') if len(files) == len(fsr4_opt.files()) else
                    _('{} of {} files missing').format(len(files), len(fsr4_opt.files())))
        self.fsr4_status.configure(text=text)
        self.fsr4_button.configure(state='normal' if (files or opt) and not self.fsr4_busy else 'disabled')

    def download_fsr4(self):
        if self.fsr4_busy:
            return
        self.fsr4_busy = True
        self.refresh_fsr4(_('Downloading…'))

        def work():
            error = None
            try:
                fsr4_opt.download(FSR4_DIR, lambda done, total: self.ui_calls.put(
                    lambda: self.refresh_fsr4(_('Downloading… {} of {}').format(done, total))))
                tools = fsr4_tools()
                if not tools:
                    raise OSError(_('spirv-cross.exe and glslangValidator.exe are missing from the bin folder'))
                fsr4_opt.build_opt(FSR4_DIR, tools, lambda done, total: self.ui_calls.put(
                    lambda: self.refresh_fsr4(_('Building the faster passes… {} of {}').format(done, total))))
            except Exception as failure:  # any failure (a dropped connection is http.client.IncompleteRead): report, re-enable
                error = failure

            def done():
                self.fsr4_busy = False
                self.refresh_fsr4()
                self.aa_box.set_options(self.aa_modes())
                if error:
                    self.messagebox.showerror('FSR 4', _('FSR 4 download failed: {}').format(error))
            self.ui_calls.put(done)
        threading.Thread(target=work, daemon=True).start()

    def update_prep_status(self):
        """The Shader preparation line for the selected game."""
        if not getattr(self, 'prep_status', None) or self.preparing:
            return
        s = self.current()
        title = next(t for k, t, _s, _e in GAMES if k == s['start'])
        prepared = shaders_prepared(s, s['start'])
        version = unsupported_version(s, s['start'])
        if version:
            text, colour = _('{}: off for this game version ({}).').format(title, version), MUTED
        elif prepared is None:
            text, colour = _('{}: no shader list for this game yet.').format(title), MUTED
        elif prepared:
            text, colour = _('{}: shaders prepared.').format(title), GOLD
        else:
            text, colour = _('{}: not prepared yet (a few minutes, once).').format(title), TEXT
        self.prep_status.configure(text=text, fg=colour)

    def prepare_again(self):
        """Forget the selected game's preparation and prepare it now."""
        if self.process:
            return
        s = self.current()
        lists = shader_list(s['start'])
        if not lists:
            return
        for folder in shader_cache_dirs(s, s['start']):
            (folder / f'prepared_{lists[0].stem}').unlink(missing_ok=True)
        self.play(prepare=True)

    def play(self, prepare=False, collection=False):
        if self.process:
            return
        if collection:  # Uncharted 1's executable with its collection menu (the skip stays off this once)
            self.var('start').set('u1')
        if prepare and not shader_list(self.current()['start']):
            self.messagebox.showinfo(NAME, _('There is no shader list for this game yet.'))
            return
        version = unsupported_version(self.current(), self.current()['start'])
        if prepare and version:
            self.messagebox.showinfo(NAME, _('Shader preparation is not available for this copy of the game '
                                             '(version {}).').format(version))
            return
        self.collect()
        folder = game_folder(self.app, self.app['start'])
        if not folder or not Path(folder).is_dir():
            self.messagebox.showerror(NAME, _('Choose the game folder with eboot.bin.'))
            self.show('game')
            return
        if not game_available(self.app, self.app['start']):
            self.messagebox.showerror(NAME, _('This game is not in the chosen folder.'))
            return
        self.set_log('')
        self.crash_log_button.pack_forget()
        env = game_environment(self.app)
        if collection:
            env.pop('BB_SKIP_COLLECTION_MENU', None)
        self.show_imported_controls()
        if prepare:
            # No window and no game (BB_PREPARE_ONLY): the progress comes back as PREP lines for the bar.
            env['BB_PREPARE_ONLY'] = '1'
            env.setdefault('BB_VARIANT_SHIP', str(shader_list(self.app['start'])[0]))
        # The launcher closing itself after the start (close_on_play): the game writes its log file
        # itself. Through a pipe nobody read once the launcher was gone, the scripts' next print
        # (a game switch in the collection menu, an in-game restart) failed and the next game
        # never started (review 2026-11-01).
        closing = bool(self.app.get('close_on_play')) and not prepare
        output, log = subprocess.PIPE, None
        if closing:
            try:
                log = open_run_log(log_location(self.app)[0])
                log.write(log_header(self.app))
                log.flush()
                output = log
            except OSError:
                output = subprocess.DEVNULL
        try:
            self.process = subprocess.Popen(run_command(), cwd=PORT_DIR, env=env,
                                            stdin=subprocess.DEVNULL, stdout=output,
                                            stderr=subprocess.STDOUT, creationflags=NO_WINDOW)
        except OSError as error:
            self.append(_('Could not start: {}').format(error) + '\n')
            self.process = None
            return
        finally:
            if log:
                log.close()  # the game has its own handle
        self.job = GameJob(self.process)
        self.entered, self.last_problem, self.stopping = False, '', False
        self.prep_shown = False
        # The preparation page closed with shader files still being saved behind the game, and what the exit left.
        self.prep_behind, self.exit_left = False, 0
        if not closing:
            self.reader = threading.Thread(target=self.read_output, args=(self.process,), daemon=True)
            self.reader.start()
        self.play_button.configure(state='disabled')
        self.stop_button.configure(state='normal')
        self.controller_refresh_button.configure(state='disabled')
        self.controller_test_button.configure(state='disabled')
        self.preparing = prepare
        if prepare:
            self.prep_bar['value'] = 0
            self.prep_bar.pack(side='left', padx=(self.px(28), 0))
            self.prep_status.configure(text=_('Preparing shaders…'), fg=GOLD)
        self.status.configure(text=_('Preparing shaders…') if prepare
                              else _('Preparing the game; it opens in its own window…'), fg=GOLD)
        if closing:
            self.root.after(5000, self.root.destroy)

    def read_output(self, process):
        """Helper thread: the game's output to the Log page and to <saves folder>/last_run.log."""
        log = None
        header = log_header(self.app)
        self.output.put(header)  # also on the Log page: copied logs name the build too
        try:
            log = open_run_log(log_location(self.app)[0])
            log.write(header)
        except OSError:
            pass
        for raw in iter(process.stdout.readline, b''):
            text = raw.decode('utf-8', errors='replace')
            if not self.closed:  # nobody shows it once the window is closed (close())
                self.output.put(text)
            # PREP lines (4 a second while shaders are prepared) feed the bar, not the log file.
            if log and not text.startswith('PREP '):
                log.write(text.replace('\r\n', '\n'))
        if log:
            log.close()
        self.output.put((process.wait(),))

    def drain_output(self):
        # One failing call (a widget a language change destroyed: TclError) must not stop this
        # pump, or the game's output and exit are never handled again.
        try:
            self.drain_once()
        except Exception as failure:  # noqa: BLE001
            print(f'launcher: {failure!r}', file=sys.stderr)
        self.root.after(100, self.drain_output)

    def drain_once(self):
        while not self.ui_calls.empty():
            try:
                self.ui_calls.get_nowait()()
            except Exception as failure:  # noqa: BLE001
                print(f'launcher: {failure!r}', file=sys.stderr)
        try:
            for _i in range(500):
                item = self.output.get_nowait()
                if isinstance(item, tuple):
                    code = item[0]
                    self.append(_('\n— the game exited (code {}) —\n').format(code))
                    self.process = None
                    if self.job:
                        self.job.close()
                    self.play_button.configure(state='normal')
                    self.stop_button.configure(state='disabled')
                    prepared, self.preparing = self.preparing, False
                    if prepared:
                        self.prep_bar.pack_forget()
                    self.refresh()
                    if import_game_controls(self.app):   # changed in the game's menu: shown here, kept
                        self.show_imported_controls()
                        self.save_settings()
                        try:
                            write_input_ini(self.app)   # (the launcher's again: a change made here now is not taken back)
                        except OSError:
                            pass
                    self.detect_controller()
                    def crashed():
                        reason = self.last_problem or _('see the Log page')
                        self.status.configure(text=_('The game stopped: {}').format(reason[:110]),
                                              fg='#e07a4f')
                        self.crash_log_button.pack(side='left', pady=12)
                        self.show('log')
                    # It crashed (not Stop, not closing the window): say why, also with files left unsaved.
                    if not self.stopping and code != 0:
                        crashed()
                    elif self.exit_left:
                        self.status.configure(text=_('{} shader files were not saved; they are built again when '
                                                     'needed.').format(self.exit_left), fg=MUTED)
                    elif prepared and code == 0 and not self.stopping:
                        self.status.configure(text=_('Shaders prepared.'), fg=GOLD)
                    elif prepared and self.stopping:
                        self.status.configure(text=_('Preparation stopped; it continues next time.'), fg=MUTED)
                    # It failed to start (not Stop, not closing the window): say why.
                    elif not self.stopping and not self.entered:
                        crashed()
                elif item.startswith('PREP '):
                    self.show_preparation(item)
                    self.prep_shown = True
                else:
                    stripped = item.strip()
                    if stripped.startswith(('STOP:', 'ERROR', 'GPU crash: the', 'Fault:')) or \
                            ('failed' in stripped and not stripped.startswith('GPU [')) or \
                            stripped.startswith(('No eboot.bin', 'No big', 'Set BB_GAME_DIR', 'bb-probe.exe not found')):
                        if not self.last_problem or stripped.startswith(('STOP:', 'GPU crash')):
                            self.last_problem = stripped
                    if 'Entering original x86-64 code' in item:
                        self.entered = True
                        self.status.configure(text=_('The game is running.'), fg=GOLD)
                        # The preparation page closed before the game code starts: its last progress line (sent
                        # 4 a second, so often short of 100 %) stood on the Shaders page all session ("Step 2 of
                        # 2 ... 60 %" on an Intel laptop, 2026-11-09). The marker follows a moment later, behind
                        # the game, so the page's end is what says the shaders are prepared.
                        if getattr(self, 'prep_shown', False) and not self.preparing:
                            self.prep_shown = False
                            title = next(t for k, t, _s, _e in GAMES if k == self.app['start'])
                            # The page closed with files still to save (a slow disk): prepared once they are saved.
                            text = _('{}: prepared; the shaders are still being saved while you play.') \
                                if self.prep_behind else _('{}: shaders prepared.')
                            self.prep_status.configure(text=text.format(title), fg=GOLD)
                    elif item.startswith('Preparation: ') and 'behind the game' in item and 'saved under' in item:
                        self.prep_behind = True
                    elif item.startswith('Preparation: every shader file saved'):
                        if self.prep_behind:
                            self.prep_behind = False
                            title = next(t for k, t, _s, _e in GAMES if k == self.app['start'])
                            self.prep_status.configure(text=_('{}: shaders prepared.').format(title), fg=GOLD)
                    elif item.startswith('Preparation: ') and 'shader files could not be saved: no prepared marker' in item:
                        # The game could not write the prepared marker (files it could not save): not prepared, so the
                        # run ends with the "files were not saved" message instead of "Shaders prepared.".
                        try:
                            self.exit_left = max(1, int(item.split()[1]))
                        except (IndexError, ValueError):
                            self.exit_left = 1
                    elif item.startswith('Shader cache at exit: '):
                        # The game is saving the shader files still queued before it ends (as long as saving goes on).
                        words = item.split()
                        if 'saving' in words or words[-1] == 'left':
                            left = next((w for w in words if w.isdigit()), '')
                            self.status.configure(text=_('Saving the prepared shaders before the game ends: {} '
                                                         'left').format(left), fg=GOLD)
                        elif 'left after' in item:
                            try:
                                self.exit_left = int(item.split('writes pending, ')[1].split()[0])
                            except (IndexError, ValueError):
                                self.exit_left = 0
                    elif item.startswith('Switching to '):
                        self.exit_left = 0   # the next game of the collection starts its own count
                        self.prep_behind = False
                        self.status.configure(text=_('Starting the next game of the collection…'), fg=GOLD)
                    self.append(item)
        except queue.Empty:
            pass

    def show_preparation(self, line):
        """A 'PREP <percent> <done> <total> <step> [<steps>]|<time left>' line from the game into the bar and status
        (<steps>: 3 when the preparation saves at the end, 2 when not; older builds send none: 2)."""
        head, _bar, left = line.rstrip('\r\n').partition('|')
        fields = head.split()[1:]
        try:
            percent, done, total, step = (int(v) for v in fields[:4])
            three = len(fields) > 4 and int(fields[4]) == 3
        except ValueError:
            return
        self.prep_bar['value'] = percent
        if three:
            steps = {0: _('Starting…'), 1: _('Step 1 of 3: reading the shaders'),
                     2: _('Step 2 of 3: compiling them for your graphics card'), 3: _('Finishing…'),
                     4: _('Step 3 of 3: saving the prepared shaders'),
                     5: _('Step 1 of 3: finding the shaders in your game files (only this once)')}
        else:
            steps = {0: _('Starting…'), 1: _('Step 1 of 2: reading the shaders'),
                     2: _('Step 2 of 2: compiling them for your graphics card'), 3: _('Finishing…'),
                     4: _('Saving the prepared shaders'),
                     5: _('Step 1 of 2: finding the shaders in your game files (only this once)')}
        text = f'{steps.get(step, "")}   {percent} %'
        if step == 5 and total:
            text += f'   ({done / 1024:.1f} / {total / 1024:.1f} GB)'  # the scan counts MiB
        elif step in (1, 2, 4) and total:
            text += f'   ({done} / {total})'
        if left.strip():
            text += f' · {left.strip()}'
        self.prep_status.configure(text=text, fg=GOLD)
        self.status.configure(text=_('Preparing shaders: {} %').format(percent) +
                              (f' · {left.strip()}' if left.strip() else ''), fg=GOLD)

    def stop(self):
        if self.job:
            self.stopping = True
            self.job.terminate()

    def append(self, text):
        self.log.configure(state='normal')
        self.log.insert('end', text)
        lines = int(self.log.index('end-1c').split('.')[0])
        if lines > MAX_LOG_LINES:
            self.log.delete('1.0', f'{lines - MAX_LOG_LINES}.0')
        self.log.see('end')
        self.log.configure(state='disabled')

    def set_log(self, text):
        self.log.configure(state='normal')
        self.log.delete('1.0', 'end')
        self.log.insert('end', text)
        self.log.configure(state='disabled')

    def open_log_folder(self):
        """The folder of last_run.log in Explorer, the log selected when there is one."""
        folder, log = log_location(self.app)
        try:
            if log:
                subprocess.Popen(f'explorer /select,"{log.resolve()}"')
            else:
                folder.mkdir(parents=True, exist_ok=True)
                os.startfile(str(folder))
        except OSError as error:  # a saves folder on a drive that is not there (an unplugged USB drive)
            self.messagebox.showerror(NAME, str(error))

    def copy_log(self):
        self.root.clipboard_clear()
        self.root.clipboard_append(self.log.get('1.0', 'end'))

    def open_path(self, path, key=None):
        if key == 'user_dir' and not path:
            path = DATA_DIR / 'user'
        path = Path(path or DATA_DIR)
        if not path.exists():
            path.mkdir(parents=True, exist_ok=True)
        os.startfile(str(path))

    # ---- release notes -------------------------------------------------------------------------
    def show_notes(self, notes=None, title=None):
        """Release notes in a window of their own: the notes' Markdown drawn simply (headings, bold, bullets,
        code); notes = [(version, text)], by default the ones this copy carries."""
        notes = release_notes() if notes is None else notes
        if not notes:
            webbrowser.open(RELEASES_PAGE)
            return
        tk = self.tk
        if getattr(self, 'notes_window', None) and self.notes_window.winfo_exists():
            self.notes_window.destroy()
        win = tk.Toplevel(self.root)
        self.notes_window = win
        win.title(title or _("What's new"))
        win.configure(bg=BG)
        win.geometry(f'{self.px(760)}x{self.px(680)}')
        win.transient(self.root)
        body = tk.Frame(win, bg=BG)
        body.pack(fill='both', expand=True, padx=self.px(16), pady=(self.px(16), self.px(10)))
        bar = tk.Scrollbar(body)
        bar.pack(side='right', fill='y')
        view = tk.Text(body, wrap='word', bg=CARD, fg=TEXT, relief='flat', highlightthickness=0, cursor='arrow',
                       padx=self.px(18), pady=self.px(14), font=self.font_ui, spacing1=self.px(2),
                       spacing3=self.px(2), yscrollcommand=bar.set)
        view.pack(side='left', fill='both', expand=True)
        bar.configure(command=view.yview)
        indent, step = self.px(10), self.px(18)
        view.tag_configure('version', foreground=GOLD, font=self.font_brand_lg, spacing1=self.px(6),
                           spacing3=self.px(8))
        view.tag_configure('heading', foreground=GOLD, font=self.font_brand_md, spacing1=self.px(12),
                           spacing3=self.px(4))
        view.tag_configure('bold', font=(self.font_ui[0], self.font_ui[1], 'bold'))
        view.tag_configure('code', font=('Consolas', self.font_ui[1]), foreground=MUTED)
        view.tag_configure('bullet', lmargin1=indent, lmargin2=indent + step)
        view.tag_configure('bullet2', lmargin1=indent + step, lmargin2=indent + 2 * step)
        view.tag_configure('note', foreground=MUTED, font=self.font_ui_sm)

        def inline(line, tags):
            line = re.sub(r'\[([^\]]+)\]\([^)]+\)', r'\1', line)  # a link: its text
            for part in re.split(r'(\*\*[^*]+\*\*|`[^`]+`)', line):
                if len(part) > 4 and part.startswith('**') and part.endswith('**'):
                    view.insert('end', part[2:-2], tags + ('bold',))
                elif len(part) > 2 and part.startswith('`') and part.endswith('`'):
                    view.insert('end', part[1:-1], tags + ('code',))
                elif part:
                    view.insert('end', part, tags)

        for i, (version, markdown) in enumerate(notes):
            view.insert('end', _('Version {}').format(version) + '\n', ('version',))
            lines = markdown.splitlines()
            for n, raw in enumerate(lines):
                line = raw.rstrip()
                if not line.strip():
                    view.insert('end', '\n')
                    continue
                if line.lstrip().startswith('#'):
                    view.insert('end', line.lstrip().lstrip('#').strip() + '\n', ('heading',))
                    continue
                bullet = re.match(r'^(\s*)[-*] (.*)$', line)
                if bullet:
                    level = 'bullet2' if len(bullet.group(1)) >= 2 else 'bullet'
                    view.insert('end', '\u2022  ' if level == 'bullet' else '\u2013  ', (level,))
                    inline(bullet.group(2).replace('\u21b3 ', ''), (level,))
                    view.insert('end', '\n', (level,))
                    continue
                # A paragraph under a bullet (indented) keeps the bullet's margin; the first lines (fork note,
                # installing) are smaller.
                tags = ('bullet2',) if raw.startswith('  ') else (('note',) if n < 4 else ())
                inline(line.strip(), tags)
                view.insert('end', '\n', tags)
            if i + 1 < len(notes):
                view.insert('end', '\n')
        view.configure(state='disabled')
        buttons = tk.Frame(win, bg=BG)
        buttons.pack(fill='x', padx=self.px(16), pady=(0, self.px(14)))
        self.pill(buttons, _('Close'), win.destroy, kind='primary').pack(side='right')
        self.pill(buttons, _('Releases page'), lambda: webbrowser.open(RELEASES_PAGE)).pack(side='right',
                                                                                          padx=(0, 8))
        # Don't show this after updates: the window then only opens from the version line or Advanced.
        hide = self.var('notes_hide_after_update')
        self.toggle(buttons, hide, background=BG).pack(side='left')
        label = tk.Label(buttons, text=_("Don't show this after updates"), bg=BG, fg=MUTED, font=self.font_ui,
                         cursor='hand2')
        label.pack(side='left', padx=(self.px(10), 0))
        label.bind('<Button-1>', lambda _e: hide.set(not hide.get()))
        if not getattr(self, 'notes_hide_traced', False):
            hide.trace_add('write', lambda *_a: self.save_settings())
            self.notes_hide_traced = True
        win.focus_set()

    def show_new_notes(self):
        """Once per version (the first start after an update or install): this version's notes."""
        self.app['notes_seen'] = VERSION
        try:
            self.collect()
        except OSError:
            pass
        current = [n for n in release_notes(1) if version_tuple(n[0]) == version_tuple(VERSION)]
        if current:
            self.show_notes(current)

    # ---- updates -------------------------------------------------------------------------------
    def check_update(self, manual=False):
        """Helper thread: asks GitHub for the newest release and offers it when it is newer."""
        try:
            version, url, page, body = latest_release()
        except (OSError, ValueError, KeyError) as failure:
            if manual:
                self.ui_calls.put(lambda error=failure: self.messagebox.showerror(
                    NAME, _('Could not check for updates: {}').format(error)))
            return

        def show():
            if version_tuple(version) > version_tuple(VERSION):
                self.offer_update(version, url, page, body)
            elif manual:
                self.messagebox.showinfo(NAME, _('You have the latest version ({}).').format(VERSION))
        self.ui_calls.put(show)

    def offer_update(self, version, url, page, body=''):
        if self.update_box and self.update_box.winfo_exists():
            return
        self.update_offer = (version, url, page, body)
        tk = self.tk
        text = _('Version {} is available.').format(version)
        if self.updating and self.update_text:
            text = self.update_text  # a download running through a language change
        box = tk.Frame(self.side, bg=CARD, highlightthickness=1, highlightbackground=GOLD_DIM)
        self.update_label = tk.Label(box, text=text, bg=CARD, fg=GOLD, font=self.font_ui_md,
                                     wraplength=self.px(190), justify='left')
        self.update_label.pack(anchor='w', padx=12, pady=(10, 8))
        buttons = tk.Frame(box, bg=CARD)
        buttons.pack(anchor='w', padx=12, pady=(0, 12))
        self.update_button = self.pill(buttons, _('Update'), lambda: self.install_update(version, url, page),
                                       kind='primary', padding=(14, 6))
        self.update_button.pack(side='left')
        if self.updating:
            self.update_button.configure(state='disabled')
        self.pill(buttons, _("What's new"),
                  lambda: self.show_notes([(version, body)]) if body else webbrowser.open(page),
                  padding=(12, 6)).pack(
            side='left', padx=(6, 0))
        box.pack(fill='x', padx=18, pady=(0, 14), before=self.side_note)
        self.update_box = box
        if not self.process:
            self.status.configure(text=_('Version {} is available.').format(version), fg=GOLD)

    def install_update(self, version, url, page):
        if self.updating:  # a version change (or this update) is downloading into UPDATE_DIR
            return
        if self.process:
            self.messagebox.showinfo(NAME, _('Close the game before updating.'))
            return
        if not FROZEN or not url:  # a source tree updates with git
            webbrowser.open(page)
            return
        if not self.messagebox.askyesno(NAME, _(
                'Install version {} now? The launcher closes, installs it and opens again. Saves and settings '
                'are kept.').format(version)):
            return
        self.update_button.configure(state='disabled')
        self.updating = True

        def progress(percent):
            self.update_text = _('Downloading version {}… {}%').format(version, percent)
            self.update_label.configure(text=self.update_text)

        def failed(error):
            self.updating, self.update_text = False, None
            self.update_button.configure(state='normal')
            self.update_label.configure(text=_('Version {} is available.').format(version))
            self.messagebox.showerror(NAME, _('Update failed: {}').format(error))

        def start(new):
            """On the window's thread: not over a game started during the download (as in change_version)."""
            if self.process:
                failed(_('Close the game before updating.'))
                return
            # The new launcher copies itself over this installation once this one has closed.
            try:
                subprocess.Popen([str(new / 'Uncharted.exe'), '--install-update', str(PORT_DIR), str(os.getpid())],
                                 cwd=str(new), stdin=subprocess.DEVNULL, creationflags=NO_WINDOW)
            except OSError as error:
                failed(error)
                return
            self.close()

        def work():
            try:
                new = download_release(url, lambda percent: self.ui_calls.put(lambda p=percent: progress(p)))
            except Exception as failure:  # every failure, not only an OSError (as in change_version)
                shutil.rmtree(UPDATE_DIR, ignore_errors=True)
                self.ui_calls.put(lambda error=failure: failed(error))
                return
            self.ui_calls.put(lambda: start(new))
        threading.Thread(target=work, daemon=True).start()

    def clear_cache(self):
        """The cache folder is renamed at once (the next start finds none) and deleted on a thread. Deleting it here
        held the window for as long as the deletion took: minutes with a large cache, and the launcher looked hung."""
        if self.process:
            return
        cache = DATA_DIR / 'user' / 'cache'
        if cache.exists():
            try:
                cache.rename(cache.with_name(f'{CLEARED_CACHE}{time.time_ns()}'))
            except OSError as error:
                self.messagebox.showerror(NAME, str(error))
                return
            threading.Thread(target=delete_cleared_caches, daemon=True).start()
        self.messagebox.showinfo(NAME, _('Shader cache cleared. The next start stutters for a while as it is rebuilt.'))

    def shortcut(self):
        """Uncharted.lnk on the desktop."""
        if FROZEN:
            target, arguments, icon = sys.executable, '', f'{sys.executable},0'
        else:
            pythonw = Path(sys.executable).with_name('pythonw.exe')
            target = str(pythonw if pythonw.exists() else sys.executable)
            arguments, icon = Path(__file__).resolve(), PORT_DIR / 'launcher' / 'uncharted.ico'
        script = ('$s=(New-Object -ComObject WScript.Shell).CreateShortcut([Environment]::GetFolderPath("Desktop")'
                  '+"\\Uncharted.lnk");'
                  f'$s.TargetPath="{target}";$s.WorkingDirectory="{PORT_DIR}";$s.IconLocation="{icon}";'
                  + (f"$s.Arguments='\"{arguments}\"';" if arguments else '') + '$s.Save()')
        result = subprocess.run(['powershell', '-NoProfile', '-Command', script], capture_output=True,
                                creationflags=NO_WINDOW)
        if result.returncode == 0:
            self.messagebox.showinfo(NAME, _('Shortcut created on the desktop.'))
        else:
            self.messagebox.showerror(NAME, result.stderr.decode(errors='replace')[:400])

    def add_to_steam(self):
        """The collection in the Steam library (a non-Steam game that starts Uncharted.exe --play)."""
        import bbport_steam
        steam = bbport_steam.steam_folder()
        configs = bbport_steam.user_configs(steam) if steam else []
        if not configs:
            self.messagebox.showerror(NAME, _('Steam was not found on this PC (or nobody has signed in to it yet).'))
            return
        if FROZEN:
            exe, options = Path(sys.executable), '--play'
        else:
            pythonw = Path(sys.executable).with_name('pythonw.exe')
            exe, options = (pythonw if pythonw.exists() else Path(sys.executable)), f'"{Path(__file__).resolve()}" --play'
        self.collect()
        restart = False
        if bbport_steam.steam_running():
            if not self.messagebox.askyesno(NAME, _('Steam has to close while the game is added; it starts '
                                                    'again afterwards. Close Steam now?')):
                return
            subprocess.Popen([str(steam / 'steam.exe'), '-shutdown'], creationflags=NO_WINDOW)
            for _i in range(60):
                time.sleep(0.5)
                if not bbport_steam.steam_running():
                    break
            else:
                self.messagebox.showerror(NAME, _('Steam did not close. Close it yourself and try again.'))
                return
            time.sleep(1)
            restart = True
        title = 'Uncharted: The Nathan Drake Collection'
        try:
            for config in configs:
                bbport_steam.add_shortcut(config / 'shortcuts.vdf', title, str(exe), str(PORT_DIR), str(exe), options)
                # Library pictures (cover, header, hero) from the box art of the player's game folder.
                try:
                    import uncharted_art
                    uncharted_art.steam_artwork(game_folder(self.app, 'u1') or game_folder(self.app, 'u2'), config / 'grid',
                                                bbport_steam.shortcut_id(f'"{exe}"', title))
                except Exception:  # pictures are a nicety: the shortcut works without them
                    pass
        except (OSError, ValueError) as error:
            self.messagebox.showerror(NAME, _('Could not add the game to Steam: {}').format(error))
            return
        if restart:
            os.startfile('steam://open/games')
        self.messagebox.showinfo(NAME, _('The collection is in your Steam library. It starts with the settings '
                                         'saved in this launcher.'))

    def save_settings(self):
        try:
            self.collect()
        except OSError:  # a locked settings file: the window keeps working
            pass

    def close(self):
        try:
            self.collect()
        except Exception:  # never keep the window open over a settings problem
            pass
        # Closed by hand while the game runs: main() keeps this process reading the game's pipe
        # (read_output, last_run.log) until the game exits. Without a reader the scripts' next
        # print failed and a game switch or in-game restart never started (the close_on_play case,
        # reviews 2026-11-01 / 2026-11-04).
        self.closed = True
        self.root.destroy()

    def wait_for_game(self):
        """After the window closed: the reader thread to its end (the game and its successors exited)."""
        if self.reader and self.reader.is_alive():
            self.reader.join()


class GameJob:
    """A Windows job holding run.py and everything it starts (the game, the next game of the
    collection, an in-game restart), so Stop ends all of them."""

    def __init__(self, process):
        kernel32 = ctypes.windll.kernel32
        kernel32.CreateJobObjectW.restype = ctypes.c_void_p
        self.handle = kernel32.CreateJobObjectW(None, None)
        if self.handle:
            kernel32.AssignProcessToJobObject(ctypes.c_void_p(self.handle), ctypes.c_void_p(int(process._handle)))

    def terminate(self):
        if self.handle:
            ctypes.windll.kernel32.TerminateJobObject(ctypes.c_void_p(self.handle), 1)

    def close(self):
        if self.handle:
            ctypes.windll.kernel32.CloseHandle(ctypes.c_void_p(self.handle))
            self.handle = None


def version_tuple(text):
    return tuple(int(number) for number in re.findall(r'\d+', text or ''))


def notes_dir():
    """The release notes this copy carries: notes/ in the package, docs/releases in a source tree."""
    for folder in (PORT_DIR / 'notes', PORT_DIR / 'docs' / 'releases'):
        if folder.is_dir():
            return folder
    return None


def release_notes(limit=5):
    """[(version, Markdown text)] of the newest release notes this copy carries, newest first."""
    folder = notes_dir()
    if not folder:
        return []
    found = sorted(((version_tuple(path.stem), path) for path in folder.glob('v*.md')
                    if 'DRAFT' not in path.name.upper()), reverse=True)
    notes = []
    for version, path in found[:limit]:
        try:
            notes.append(('.'.join(str(n) for n in version), path.read_text(encoding='utf-8')))
        except OSError:
            pass
    return notes


def github_json(url):
    """The JSON answer of a GitHub API request. A connection that ends partway is an OSError too (http.client's
    IncompleteRead is not one, and ended the helper thread that asked)."""
    request = urllib.request.Request(url, headers={'Accept': 'application/vnd.github+json',
                                                   'User-Agent': 'uncharted-launcher'})
    try:
        with urllib.request.urlopen(request, timeout=15) as response:
            return json.load(response)
    except http.client.HTTPException as error:
        raise OSError(f'the answer from GitHub stopped partway ({error!r})') from error


def release_zip(release):
    """The download URL of a GitHub release's zip (None without one)."""
    return next((asset.get('browser_download_url') for asset in release.get('assets') or []
                 if isinstance(asset, dict) and str(asset.get('name') or '').lower().endswith('.zip')), None)


def latest_release():
    """(version, zip URL, page URL, release notes) of the newest GitHub release."""
    release = github_json(RELEASES_API)
    version = '.'.join(re.findall(r'\d+', release['tag_name']))
    return version, release_zip(release), release.get('html_url') or RELEASES_PAGE, release.get('body') or ''


def release_choices(releases, test_builds=False, oldest=OLDEST_SWITCH_VERSION):
    """The releases Advanced > Version offers, from GitHub's release list: [(name, zip URL, page URL, test build)],
    newest first. name is the tag without its 'v' (a test build keeps its suffix: 0.3.6.58.1-test-reflex). Left out:
    drafts, releases without a zip, ones older than oldest, and test builds (GitHub pre-releases) unless test_builds.
    A release comes before the test builds with the same number."""
    found = []
    for release in releases:
        if not isinstance(release, dict):
            continue
        tag, url = str(release.get('tag_name') or ''), release_zip(release)
        test = bool(release.get('prerelease'))
        if release.get('draft') or not url or version_tuple(tag) < oldest or (test and not test_builds):
            continue
        found.append((tag[1:] if tag[:1] in 'vV' else tag, url, release.get('html_url') or RELEASES_PAGE, test))
    return sorted(found, key=lambda r: (version_tuple(r[0]), not r[3]), reverse=True)


def download_release(url, progress):
    """Downloads a release's zip into UPDATE_DIR and unpacks it there; returns the folder that holds its
    Uncharted.exe. progress(percent) is called on this thread as the download goes on. Every failure is an OSError
    (a cut connection, a damaged zip: zlib.error, EOFError and http.client errors are not) and leaves nothing in
    UPDATE_DIR."""
    shutil.rmtree(UPDATE_DIR, ignore_errors=True)
    try:
        UPDATE_DIR.mkdir(parents=True, exist_ok=True)
        archive = UPDATE_DIR / 'update.zip'
        request = urllib.request.Request(url, headers={'User-Agent': 'uncharted-launcher'})
        with urllib.request.urlopen(request, timeout=60) as response, open(archive, 'wb') as out:
            length = str(response.headers.get('Content-Length') or '')
            total, done, shown = int(length) if length.isdigit() else 0, 0, -1  # a malformed size: no percentage
            while chunk := response.read(1 << 20):
                out.write(chunk)
                done += len(chunk)
                percent = done * 100 // total if total else 0
                if percent != shown:
                    shown = percent
                    progress(percent)
        if total and done != total:  # the connection ended early (read returns no error for that)
            raise OSError(f'the download stopped after {done >> 20} of {total >> 20} MB')
        try:
            with zipfile.ZipFile(archive) as package:
                package.extractall(UPDATE_DIR / 'new')
        except (zipfile.BadZipFile, zlib.error, EOFError, ValueError, NotImplementedError, RuntimeError) as error:
            # NotImplementedError: a compression method zipfile has not; RuntimeError: an encrypted member
            raise OSError(f'the download is damaged ({error})') from error
        archive.unlink()
        new = next((p.parent for p in (UPDATE_DIR / 'new').rglob('Uncharted.exe')), None)
        if not new:
            raise OSError('Uncharted.exe is missing from the download')
        return new
    except http.client.HTTPException as error:
        shutil.rmtree(UPDATE_DIR, ignore_errors=True)
        raise OSError(f'the download stopped ({error!r})') from error
    except BaseException:
        shutil.rmtree(UPDATE_DIR, ignore_errors=True)
        raise


def wait_for_process(pid):
    """Waits (up to a minute) for the process pid to end."""
    kernel = ctypes.windll.kernel32
    handle = kernel.OpenProcess(0x00100000, False, int(pid))  # SYNCHRONIZE
    if handle:
        kernel.WaitForSingleObject(handle, 60000)
        kernel.CloseHandle(handle)


def release_name(folder):
    """The exact release the package in folder is, from its RELEASE_FILE (the tag without its 'v', as
    0.3.6.65.1-test); None without one."""
    try:
        tag = (Path(folder) / RELEASE_FILE).read_text(encoding='utf-8').strip()
    except (OSError, ValueError):
        return None
    return (tag[1:] if tag[:1] in 'vV' else tag) or None


def installed_release():
    """The exact release installed: release_name(PORT_DIR) when it is one of VERSION (a release or a test build of
    that number), else VERSION (a build made without a tag)."""
    name = release_name(PORT_DIR)
    return name if name and (name == VERSION or name.startswith(VERSION + '-')) else VERSION


def backup_folder_name(version):
    """The folder in versions/ that keeps a release: its name, with anything a folder name cannot hold replaced."""
    return re.sub(r'[^0-9A-Za-z._-]', '_', version) or 'unknown'


def version_backups(folder=None):
    """[(version, path)] of the releases kept in versions/, the most recently left first (version: the exact release
    from its RELEASE_FILE, else the folder's name). Only a whole one counts
    (whole_release: not one a stopped switch or a deletion left half made)."""
    folder = Path(folder or VERSIONS_DIR)
    try:
        found = [p for p in folder.iterdir() if p.is_dir() and p.name != INCOMING_DIR and whole_release(p)]
    except OSError:
        return []

    def left(path):  # one deleted since the listing sorts last (version_backups runs while the page is built)
        try:
            return path.stat().st_mtime
        except OSError:
            return 0
    return [(release_name(p) or p.name, p) for p in sorted(found, key=left, reverse=True)]


def whole_release(folder):
    """Whether folder holds what a release needs to start (WHOLE_RELEASE). switch_files moves Uncharted.exe last,
    so a backup it stopped making partway has none."""
    folder = Path(folder)
    return (folder / 'Uncharted.exe').is_file() and all((folder / name).is_dir() for name in WHOLE_RELEASE[1:])


def prune_backups(folder=None, keep=KEEP_BACKUPS):
    """Deletes all but the keep most recent backups in versions/, and anything else in it that is no backup."""
    folder = Path(folder or VERSIONS_DIR)
    kept = {path for _v, path in version_backups(folder)[:keep]}
    try:
        entries = list(folder.iterdir())
    except OSError:
        return
    for path in entries:
        if path in kept:
            continue
        if path.is_dir():
            remove_tree(path, ignore_errors=True)
        else:
            try:
                path.unlink(missing_ok=True)
            except OSError:  # a locked or read-only file: left, as remove_tree(ignore_errors) leaves one
                pass


def folders_inside(settings, roots):
    """The player's folders in settings (PLAYER_FOLDERS) that are one of roots or lie inside one (compared as Windows
    does: case and / or \\ aside, links followed)."""
    def norm(path):
        return Path(os.path.normcase(os.path.realpath(path)))
    roots = [norm(root) for root in roots]
    found = []
    for key in PLAYER_FOLDERS:
        value = str(settings.get(key) or '').strip()
        if value and any(norm(value).is_relative_to(root) for root in roots):
            found.append(value)
    return found


def folder_empty(folder):
    """Whether folder has nothing in it (or is not there)."""
    try:
        return not any(Path(folder).iterdir())
    except OSError:
        return True


def remove_tree(folder, ignore_errors=False):
    """shutil.rmtree that also deletes read-only files (shutil.rmtree stops at one with access denied)."""
    def writable(function, path, _error):
        os.chmod(path, stat.S_IWRITE)
        function(path)
    try:
        shutil.rmtree(folder, onexc=writable)
    except OSError:
        if not ignore_errors:
            raise


def folder_size(folder):
    """The bytes of every file under folder (0 without it)."""
    total = 0
    for root, _dirs, files in os.walk(folder):
        for name in files:
            try:
                total += os.path.getsize(os.path.join(root, name))
            except OSError:
                pass
    return total


def remove_backups(folder=None):
    """Deletes versions/ with every release kept in it; raises when something in it could not be deleted."""
    folder = Path(folder or VERSIONS_DIR)
    if folder.exists():
        remove_tree(folder)


def same_drive(a, b):
    """Whether a and b are on one volume, so that a rename between them moves nothing."""
    try:
        return os.stat(a).st_dev == os.stat(b).st_dev
    except OSError:
        return False


def stage_release(source, versions):
    """The release in source on the installation's drive, for switch_files (which only renames): source itself when
    it is there already (a kept backup, a download with %TEMP% on the same drive), else a copy of its program files in
    versions/INCOMING_DIR (%TEMP% on C:, the port on D:). A failed copy is removed and raised; the installation is not
    touched either way."""
    source, versions = Path(source), Path(versions)
    versions.mkdir(parents=True, exist_ok=True)
    if same_drive(source, versions):
        return source
    staged = versions / INCOMING_DIR
    remove_tree(staged, ignore_errors=True)  # what a switch stopped partway left
    try:
        staged.mkdir()
        for entry in source.iterdir():
            if entry.name in SWITCH_KEEP:
                continue
            if entry.is_dir():
                shutil.copytree(entry, staged / entry.name)
            else:
                shutil.copy2(entry, staged / entry.name)
    except OSError:
        shutil.rmtree(staged, ignore_errors=True)
        raise
    return staged


class SwitchIncomplete(OSError):
    """A switch that failed and could not move every file back: the files of the release that was installed
    (old) and of the one coming in (new) are split over folders, {folder: [names]} each. Never tried again, and
    nothing of it is deleted (a second attempt would delete the backup, the only copy of what stayed in it)."""
    def __init__(self, cause, old, new):
        super().__init__(f'{cause}; not every file could be moved back')
        self.cause, self.old, self.new = cause, old, new


def rename_retried(source, destination, tries=5):
    """os.rename, tried again for a moment (tries x 0.2 s) while a file in it is held, as by an antivirus scan of a
    folder just renamed. Returns whether it moved."""
    for attempt in range(tries):
        try:
            os.rename(source, destination)
            return True
        except OSError:
            if attempt + 1 < tries:
                time.sleep(0.2)
    return False


def switch_files(target, source, backup):
    """Moves the release installed in target (its program files, never SWITCH_KEEP) into backup, then moves the
    release in source (a kept backup or stage_release's folder, on target's drive) into target. Every move is a rename
    (os.rename: all or nothing, never a copy; shutil.move copied a folder it could not rename, as one with a file in
    use, and then deleted it from target partway). On a failure every moved entry goes back where it was and the error
    is raised, so target holds one whole release either way; when a move back fails too, SwitchIncomplete says where
    each release's files are, and nothing is deleted. source is removed once empty."""
    target, source, backup = Path(target), Path(source), Path(backup)
    if not whole_release(source):
        missing = [name for name in WHOLE_RELEASE if not (source / name).exists()]
        raise OSError(f"{', '.join(missing) or 'Uncharted.exe'} is missing from {source}")
    # Uncharted.exe moves last: a backup a stopped switch left half made has none (whole_release).
    last = lambda name: (name == 'Uncharted.exe', name)  # noqa: E731
    incoming = sorted((p.name for p in source.iterdir() if p.name not in SWITCH_KEEP), key=last)
    leaving = sorted((name for name in set(PROGRAM_FILES) | set(incoming)
                      if name not in SWITCH_KEEP and (target / name).exists()), key=last)
    if backup.exists():
        remove_tree(backup)  # an earlier backup of the same release
    backup.mkdir(parents=True)
    moved, arrived = [], []
    try:
        for name in leaving:
            os.rename(target / name, backup / name)
            moved.append(name)
        for name in incoming:
            os.rename(source / name, target / name)
            arrived.append(name)
    except OSError as failure:
        new_stuck = [name for name in reversed(arrived) if not rename_retried(target / name, source / name)]
        old_stuck = [name for name in reversed(moved) if not rename_retried(backup / name, target / name)]
        if new_stuck or old_stuck:
            old = {target: [name for name in leaving if name not in old_stuck], backup: old_stuck}
            new = {target: new_stuck, source: [name for name in incoming if name not in new_stuck]}
            raise SwitchIncomplete(failure, *({folder: sorted(names) for folder, names in where.items() if names}
                                              for where in (old, new))) from failure
        try:
            backup.rmdir()  # empty again (never rmtree: it held the release a moment ago)
        except OSError:
            pass
        raise
    try:
        os.utime(backup)  # the time version_backups orders the backups by: now, the moment this release was left
    except OSError as error:  # the switch itself is done
        switch_log(f'could not set the time of {backup}: {error}')
    try:
        source.rmdir()
    except OSError:
        pass


def switch_version(target, source, version, wait_pid):
    """--switch-version TARGET SOURCE VERSION PID, run by a copy of the launcher in UPDATE_DIR (none of its own files
    are in TARGET): waits for the launcher to close, moves the installed release VERSION into
    TARGET/versions/VERSION and the one in SOURCE into TARGET (switch_files), keeps the most recent backup and starts
    the launcher in TARGET. The saves, settings and shader cache are not touched. A download on another drive is
    copied next to the installation first (stage_release), so that the switch itself only renames."""
    target = Path(target)
    wait_for_process(wait_pid)
    backup = target / 'versions' / backup_folder_name(version)
    error = None
    try:
        staged = stage_release(source, target / 'versions')
    except OSError as failure:  # the copy to the installation's drive (a full disk): nothing was changed
        staged, error = None, failure
    for _attempt in range(30 if staged else 0):
        try:
            switch_files(target, staged, backup)
            error = None
            if Path(staged) != Path(source):
                shutil.rmtree(source, ignore_errors=True)  # the download, copied in
            break
        except SwitchIncomplete as failure:  # the files are split: not tried again, nothing deleted, nothing started
            ctypes.windll.user32.MessageBoxW(None, incomplete_message(failure, version, target), NAME, 0x10)
            return 1
        except PermissionError as failure:  # a file still in use (the launcher is closing): tried again
            error = failure
            time.sleep(1)
        except OSError as failure:  # anything else (a full disk): not tried again
            error = failure
            break
    if error:
        if staged and Path(staged) != Path(source):
            shutil.rmtree(staged, ignore_errors=True)
        ctypes.windll.user32.MessageBoxW(None, _('Could not change the version: {}').format(error), NAME, 0x10)
        start_launcher(target)
        return 1
    # The switch is done: what fails from here on is logged and changes neither the restart nor the result.
    try:
        prune_backups(target / 'versions')
    except OSError as error:
        switch_log(f'could not remove the older backups: {error}')
    start_launcher(target)
    return 0


def switch_log(text):
    """A line in SWITCH_LOG."""
    try:
        with open(SWITCH_LOG, 'a', encoding='utf-8') as log:
            log.write(f"{time.strftime('%Y-%m-%d %H:%M:%S')} {text}\n")
    except OSError:
        pass


def start_launcher(target):
    """The switcher's last step: the launcher in target started (a failure only logged)."""
    try:
        subprocess.Popen([str(target / 'Uncharted.exe')], cwd=str(target))
    except OSError as error:
        switch_log(f"could not start {target / 'Uncharted.exe'}: {error}")


def incomplete_message(failure, version, target):
    """The switcher's message for SwitchIncomplete: which folder holds which release's files."""
    lines = [_('Could not change the version, and not every file could be moved back: {}').format(failure.cause), '']
    lines += [_('Files of version {} (the one installed before), in {}: {}').format(version, folder, ', '.join(names))
              for folder, names in failure.old.items()]
    lines += [_('Files of the version you were changing to, in {}: {}').format(folder, ', '.join(names))
              for folder, names in failure.new.items()]
    return '\n'.join(lines + ['', _('Nothing was deleted. To use version {} again, move its files back into {}.'
                                    ).format(version, target)])


def install_update(target, wait_pid):
    """--install-update TARGET PID, run by the downloaded version from its temporary folder:
    waits for the old launcher to close, copies this version over TARGET (never the saves,
    settings or art) and starts it."""
    target = Path(target)
    wait_for_process(wait_pid)
    ignore = shutil.ignore_patterns(*USER_FILES)
    if not (PORT_DIR / RELEASE_FILE).exists():  # the copy-over would leave the old release's one
        try:
            (target / RELEASE_FILE).unlink(missing_ok=True)
        except OSError:
            pass  # installed_release() still checks it against VERSION
    for attempt in range(30):
        try:
            shutil.copytree(PORT_DIR, target, dirs_exist_ok=True, ignore=ignore)
            break
        except OSError:  # a file still in use: the old launcher is closing
            time.sleep(1)
    else:
        ctypes.windll.user32.MessageBoxW(None, _('Could not install the update. Download it from the '
                                                 'releases page.'), NAME, 0x10)
        webbrowser.open(RELEASES_PAGE)
        return 1
    subprocess.Popen([str(target / 'Uncharted.exe')], cwd=str(target))
    return 0


def log_header(settings, start=None):
    """The first line of last_run.log: which build, from which folder, started which game (the
    first question about any report)."""
    try:
        commit = (PORT_DIR / 'commit.txt').read_text(encoding='utf-8').strip()
    except OSError:
        commit = ''
    build = f'{VERSION} ({commit})' if commit else VERSION
    saves = Path(settings.get('user_dir') or DATA_DIR / 'user')
    start = start or settings.get('start')
    info = game_info(game_folder(settings, start)) if start else None
    game = f"Game: {info['id']} version {info['version']}" if info else 'Game: folder not found'
    if info:
        code = game_code(settings, start)
        lists = shader_list(start)
        locations = shader_locations(lists, settings, start) if lists else None
        game += f' (game code {code})' if code else ' (game code unknown)' if code == '' else ''
        game += (f'; shader locations {locations.name}' if locations
                 else '; no shipped shader index: the game files are scanned once (Advanced override)'
                 if needs_index_scan(settings, start)
                 else '; shader preparation off: no index for it')
    return (f'Port: {NAME} {build} in {PORT_DIR}, starting {start}\n'
            f'Folders: saves {saves}, shader cache {DATA_DIR / "user" / "cache"}\n{game}\n')


def log_location(settings):
    """(folder, file) of last_run.log: the folder is the saves folder (Game page), else user/ next to the
    launcher; file is its last_run.log, None while there is none yet."""
    folder = Path(settings.get('user_dir') or DATA_DIR / 'user')
    log = folder / 'last_run.log'
    return folder, log if log.is_file() else None


def open_run_log(log_dir, keep=5):
    """last_run.log for a new run; the previous ones move to last_run.1.log ... last_run.<keep-1>.log
    (a restart after a problem no longer overwrites the log that shows it)."""
    log_dir.mkdir(parents=True, exist_ok=True)
    for n in range(keep - 1, 0, -1):
        older = log_dir / (f'last_run.{n - 1}.log' if n > 1 else 'last_run.log')
        try:
            if older.exists():
                older.replace(log_dir / f'last_run.{n}.log')
        except OSError:
            pass
    return open(log_dir / 'last_run.log', 'w', encoding='utf-8', buffering=1)


def play_without_window(settings):
    """--play [u1|u2|u3]: the game with the saved settings (shortcuts, Steam); output to the console
    when there is one and to <saves folder>/last_run.log."""
    attach_stdio()
    with open_run_log(log_location(settings)[0]) as log:
        log.write(log_header(settings))
        process = subprocess.Popen(run_command(), cwd=PORT_DIR, env=game_environment(settings),
                                   stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                   stderr=subprocess.STDOUT, creationflags=NO_WINDOW)
        for raw in iter(process.stdout.readline, b''):
            text = raw.decode('utf-8', errors='replace')
            if not text.startswith('PREP '):  # the bar's lines, 4 a second while preparing
                log.write(text)
            try:
                sys.stdout.write(text)
            except (OSError, ValueError):
                pass
        return process.wait()


def main():
    args = sys.argv[1:]
    if args and args[0] in ('--run', '--script'):
        sys.exit(run_role(args))
    if args[:1] == ['--install-update'] and len(args) == 3:
        sys.exit(install_update(args[1], args[2]))
    if args[:1] == ['--switch-version'] and len(args) == 5:
        sys.exit(switch_version(*args[1:]))
    if FROZEN and UPDATE_DIR not in PORT_DIR.parents:
        shutil.rmtree(UPDATE_DIR, ignore_errors=True)  # what a finished update left behind
    settings = load_settings()
    if '--play' in args:
        # --play u1|u2|u3 (front ends such as Playnite: one entry per game) for this start only;
        # /u2, -u2 and --u2 work as well.
        chosen = [arg.lstrip('/-').lower() for arg in args[args.index('--play') + 1:]]
        game = next((arg for arg in chosen if arg in (key for key, *_rest in GAMES)), None)
        if game:
            settings['start'] = game
    if '--play' in args and game_available(settings, settings['start']):
        sys.exit(play_without_window(settings))
    try:
        ctypes.windll.shcore.SetProcessDpiAwareness(1)
    except (AttributeError, OSError):
        pass
    import tkinter as tk
    from tkinter import filedialog, messagebox, ttk
    root = tk.Tk()
    launcher = Launcher(root, tk, ttk, filedialog, messagebox)
    root.mainloop()
    launcher.wait_for_game()


if __name__ == '__main__':
    main()
